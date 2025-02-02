// SPDX-License-Identifier: Apache-2.0
#include "xstudio/media_reader/media_reader.hpp"
#include "xstudio/ui/opengl/no_image_shader_program.hpp"
#include "xstudio/ui/opengl/shader_program_base.hpp"
#include "xstudio/ui/opengl/texture.hpp"
#include "xstudio/ui/opengl/opengl_viewport_renderer.hpp"
#include "xstudio/utility/logging.hpp"
#include "xstudio/utility/uuid.hpp"

#ifdef DEBUG_GRAB_FRAMEBUFFER
#include "xstudio/ui/opengl/gl_debug_utils.h"
#endif

using namespace xstudio;
using namespace xstudio::ui::opengl;
using namespace xstudio::ui::viewport;
using namespace xstudio::media_reader;
using namespace xstudio::colour_pipeline;
using namespace xstudio::utility;

namespace {

static const std::string default_vertex_shader = R"(
#version 330 core
vec2 calc_pixel_coordinate(vec2 viewport_coordinate)
{
    return vec2(viewport_coordinate.x+1.0,1.0-viewport_coordinate.y)*0.5;
}
)";

static const std::string colour_transforms = R"(
vec4 colour_transforms(vec4 rgba_in)
{
    return rgba_in;
}
)";

} // namespace

using nlohmann::json;


ColourPipeLutCollection::ColourPipeLutCollection(const ColourPipeLutCollection &o) {
    lut_textures_ = o.lut_textures_;
}

void ColourPipeLutCollection::upload_luts(
    const std::vector<colour_pipeline::ColourLUTPtr> &luts, const bool is_main_viewer) {

    active_luts_.clear();

    for (const auto &lut : luts) {

        if (((lut->target_viewer() & ColourLUT::MAIN_VIEWER) && is_main_viewer) ||
            ((lut->target_viewer() & ColourLUT::POPOUT_VIEWER) && !is_main_viewer)) {
            if (lut_textures_.find(lut->texture_name_and_desc()) == lut_textures_.end()) {
                lut_textures_[lut->texture_name_and_desc()].reset(
                    new GLColourLutTexture(lut->descriptor(), lut->texture_name()));
            }
            lut_textures_[lut->texture_name_and_desc()]->upload_texture_data(lut);
            active_luts_.push_back(lut_textures_[lut->texture_name_and_desc()]);
        }
    }
}

void ColourPipeLutCollection::bind_luts(GLShaderProgramPtr shader, int &tex_idx) {
    for (const auto &lut : active_luts_) {
        utility::JsonStore txshder_param(nlohmann::json{{lut->texture_name(), tex_idx}});
        lut->bind(tex_idx);
        shader->set_shader_parameters(txshder_param);
        tex_idx++;
    }
}

OpenGLViewportRenderer::OpenGLViewportRenderer(
    const bool is_main_viewer, const bool gl_context_shared)
    : viewport::ViewportRenderer(),
      gl_context_shared_(gl_context_shared),
      is_main_viewer_(is_main_viewer) {}

void OpenGLViewportRenderer::upload_image_and_colour_data(
    std::vector<media_reader::ImageBufPtr> next_images) {


    if (!next_images.size()) {
        if (onscreen_frame_)
            onscreen_frame_.reset();
        active_shader_program_ = no_image_shader_program_;
        return;
    }

    onscreen_frame_                                         = next_images.front();
    colour_pipeline::ColourPipelineDataPtr colour_pipe_data = onscreen_frame_.colour_pipe_data_;

    if (!textures_.size())
        return;

    textures_[0]->set_texture_type("SSBO"); // texture_mode_preference_->value());

    if (onscreen_frame_) {
        if (onscreen_frame_->error_state() == BufferErrorState::HAS_ERROR) {
            // the frame contains errors, no need to continue from that point
            return;
        }

        // check if the frame we need to draw has already been
        // uploaded to texture memory and set the 'draw_texture_index_'
        // accordingly
        textures_[0]->upload_next(next_images);
    }

    if (colour_pipe_data && colour_pipe_data->cache_id_ != latest_colour_pipe_data_cacheid_) {
        colour_pipe_textures_.upload_luts(colour_pipe_data->luts_, is_main_viewer_);
        latest_colour_pipe_data_cacheid_ = colour_pipe_data->cache_id_;
    }

    if (onscreen_frame_ && colour_pipe_data &&
        activate_shader(
            onscreen_frame_->shader(),
            is_main_viewer_ ? colour_pipe_data->main_viewport_shader_
                            : colour_pipe_data->popout_viewport_shader_)) {

        active_shader_program_->set_shader_parameters(onscreen_frame_, is_main_viewer_);

        active_shader_program_->set_shader_parameters(colour_pipe_data.shader_parameters_);

    } else {
        active_shader_program_ = no_image_shader_program_;
    }

    bind_textures();
}

void OpenGLViewportRenderer::bind_textures() {

    if (!active_shader_program_ || active_shader_program_ == no_image_shader_program_)
        return;

    int tex_idx     = 1;
    bool using_ssbo = false;
    Imath::V2i tex_dims;
    textures_[0]->bind(tex_idx, tex_dims, using_ssbo);
    utility::JsonStore txshder_param;
    txshder_param["the_tex"]  = tex_idx;
    txshder_param["tex_dims"] = tex_dims;
    txshder_param["use_ssbo"] = using_ssbo;

    tex_idx++;

    active_shader_program_->set_shader_parameters(txshder_param);
    colour_pipe_textures_.bind_luts(active_shader_program_, tex_idx);
}

void OpenGLViewportRenderer::release_textures() {

    if (active_shader_program_ == no_image_shader_program_)
        return;

    textures_[0]->release();
}

// static std::mutex m;

void OpenGLViewportRenderer::clear_viewport_area(const Imath::M44f &to_scene_matrix) {

    // The GL viewport does not necessarily match the area of the xstudio
    // viewport ... for example, the xstudio viewport is typically embedded in
    // a QML interface with various other widgets arranged around it. At render
    // time, we take account of this using the 'to_scene_matrix' which maps from
    // the current GL_VIEWPORT to the area of the xstudio viewport within it.
    // In this case, we are using this to run glClear but only on the area of
    // the viewport. This might save a tiny bit of time when rendering the whole
    // interface, but also means we don't stomp on something that has been drawn
    // into the GL canvas that is supposed to be there!
    std::array<int, 4> vp;
    glGetIntegerv(GL_VIEWPORT, vp.data());

    Imath::V4f botomleft(0.0, 0.0, 0.0f, 1.0f);
    Imath::V4f topright(1.0, 1.0, 0.0f, 1.0f);

    topright  = topright * to_scene_matrix;
    botomleft = botomleft * to_scene_matrix;

    const int xs_vp_left   = (int)round(botomleft.x * vp[2] / botomleft.w);
    const int xs_vp_bottom = vp[3] - (int)round(topright.y * vp[3] / topright.w);

    const int xs_vp_right = (int)round(topright.x * vp[2] / topright.w);
    const int xs_vp_top   = vp[3] - (int)round(botomleft.y * vp[3] / botomleft.w);

    // glEnable(GL_SCISSOR_TEST);
    // glScissor(xs_vp_left, xs_vp_bottom, xs_vp_right-xs_vp_left, xs_vp_top-xs_vp_bottom);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    // glDisable(GL_SCISSOR_TEST);
}

void OpenGLViewportRenderer::render(
    const std::vector<media_reader::ImageBufPtr> &next_images,
    const Imath::M44f &to_scene_matrix,
    const Imath::M44f &projection_matrix,
    const Imath::M44f &fit_mode_matrix) {


    // const std::lock_guard<std::mutex> mutex_locker(m);
    init();

    const auto transform_viewport_to_image_space =
        projection_matrix * fit_mode_matrix.inverse();

    // this value tells us how much we are zoomed into the image in the viewport (in
    // the x dimension). If the image is width-fitted exactly to the viewport, then this
    // value will be 1.0 (what it means is the coordinates -1.0 to 1.0 are mapped to
    // the width of the viewport)
    const float image_zoom_in_viewport = transform_viewport_to_image_space[0][0];


    // this value gives us how much of the parent window is covered by the viewport.
    // So if the xstudio window is 1000px in width, and the viewport is 500px wide
    // (with the rest of the UI taking up the remainder) then this value will be 0.5
    const float viewport_x_size_in_window = to_scene_matrix[0][0] / to_scene_matrix[3][3];


    // the gl viewport corresponds to the parent window size.
    std::array<int, 4> gl_viewport;
    glGetIntegerv(GL_VIEWPORT, gl_viewport.data());
    const auto viewport_width = (float)gl_viewport[2];

    // this value tells us how much a screen pixel width in the viewport is in the units
    // of viewport coordinate space
    const float viewport_du_dx =
        image_zoom_in_viewport / (viewport_width * viewport_x_size_in_window);

    /* we do our own clear of the viewport */
    clear_viewport_area(to_scene_matrix);

    // if we've received a new image and/or colour pipeline data (LUTs etc) since the last
    // draw, upload the data
    upload_image_and_colour_data(next_images);

    glUseProgram(0);

    /* Call the render functions of overlay plugins - for the BeforeImage pass, we only call
    this if we have an alpha buffer that allows us to 'under' the image with the overlay
    drawings. */
    if (onscreen_frame_ && has_alpha_) {
        for (auto orf : viewport_overlay_renderers_) {
            if (orf.second->preferred_render_pass() ==
                plugin::ViewportOverlayRenderer::BeforeImage) {
                orf.second->render_opengl(
                    to_scene_matrix,
                    transform_viewport_to_image_space,
                    onscreen_frame_,
                    has_alpha_);
            }
        }
    }

    glDisable(GL_DEPTH_TEST);

    glEnable(GL_BLEND);
    glBlendFunc(has_alpha_ ? GL_ONE_MINUS_DST_ALPHA : GL_ONE, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);

    if (active_shader_program_) {

        active_shader_program_->use();

        bool use_bilinear_filtering = false;
        if (onscreen_frame_) {
            // here we can work out the ratio of image pixels to screen pixels
            const float image_pix_to_screen_pix =
                onscreen_frame_->image_size_in_pixels().x * viewport_du_dx;
            if (render_hints_ == AlwaysBilinear)
                use_bilinear_filtering =
                    image_pix_to_screen_pix < 0.99999f || image_pix_to_screen_pix > 1.00001f;
            else if (render_hints_ == BilinearWhenZoomedOut)
                use_bilinear_filtering =
                    image_pix_to_screen_pix < 0.99999f; // filter_mode_ == BilinearWhenZoomedOut
        }

        // coordinate system set-up
        utility::JsonStore shader_params;
        shader_params["to_coord_system"]        = transform_viewport_to_image_space;
        shader_params["to_canvas"]              = to_scene_matrix;
        shader_params["use_bilinear_filtering"] = use_bilinear_filtering;
        shader_params["pixel_aspect"] =
            onscreen_frame_ ? onscreen_frame_->pixel_aspect() : 1.0f;
        active_shader_program_->set_shader_parameters(shader_params);

        // The quad that we draw simply fills the viewport area. Note the projection and model
        // matrices are identity at draw time.
        // In this case coord -1.0,-1.0 maps to bottom left and 1.0,1.0
        // maps to top right of the gl_viewport.
        //
        // However, when rendering in qml as a QQuickItem the gl_viewport is currently set to
        // the shape of the QQuickWindow that contains the whole application UI - to transform
        // to the coordinates of the Viewport QQuickItem, we multiply by the "to_canvas" matrix,
        // which is done in the main shader.
        static std::array<float, 16> vertices = {
            -1.0,
            1.0,
            0.0f,
            1.0f,
            1.0,
            1.0,
            0.0f,
            1.0f,
            1.0,
            -1.0,
            0.0f,
            1.0f,
            -1.0,
            -1.0,
            0.0f,
            1.0f};

        glBindVertexArray(vao_);
        // 2. copy our vertices array in a buffer for OpenGL to use
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
        // 3. then set our vertex module pointers
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
        glEnableVertexAttribArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    glBindVertexArray(vao_);
    glDrawArrays(GL_QUADS, 0, 4);
    glUseProgram(0);


    /* N.B. To draw into the image coordinate system (where image with identity transform has
    spans of -1.0 to 1.0 and is centred on 0,0): Imath::M44f r =
    to_coord_sys.inverse()*to_canvas; glMatrixMode(GL_MODELVIEW_MATRIX); glMultMatrixf(r[0]);
    */

    /* N.B. this glfinish is required to keep the popout viewport in sync with the main viewport
    because the two may viewports share textures, shaders and other resources (they can be the
    same GL context)* - without this segfault in graphics driver is possible */
    if (gl_context_shared_)
        glFinish();

    release_textures();

    /* Call the render functions of overlay plugins - note that if the overlay prefers to draw
    before the image but we have no alpha channel, we still call its render function here */
    if (onscreen_frame_) {
        for (auto orf : viewport_overlay_renderers_) {
            if (orf.second->preferred_render_pass() ==
                    plugin::ViewportOverlayRenderer::AfterImage ||
                !has_alpha_) {
                orf.second->render_opengl(
                    to_scene_matrix,
                    transform_viewport_to_image_space,
                    onscreen_frame_,
                    has_alpha_);
            }
        }
    }

#ifdef DEBUG_GRAB_FRAMEBUFFER
    grab_framebuffer_to_disk();
#endif

    // The use of this is questionable - I'm not sure how it plays with the QML
    // rendering engine. The idea is that we are guaranteeing the image has
    // been rendered at this stage so we are sure it's on screen for the next
    // refresh. In testing, it seems to help smooth playback under PCOIP when
    // doing demanding rendering - e.g. high frame rate, high res sources with
    // bilinear filtering.
    // glFinish();
}

bool OpenGLViewportRenderer::activate_shader(
    const viewport::GPUShaderPtr &image_buffer_unpack_shader,
    const viewport::GPUShaderPtr &colour_pipeline_shader) {

    if (!image_buffer_unpack_shader) {
        spdlog::warn("{} {}", __PRETTY_FUNCTION__, "No shader passed with image buffer.");
        return false;
    }

    if (!colour_pipeline_shader) {
        spdlog::warn(
            "{} {}", __PRETTY_FUNCTION__, "No shader passed with colour pipeline LUTs.");
        return false;
    }

    const auto &ib_sid = image_buffer_unpack_shader->shader_id_;
    const auto &cp_sid = colour_pipeline_shader->shader_id_;

    // do we already have this shader compiled?
    if (programs_.find(ib_sid) == programs_.end() ||
        programs_[ib_sid].find(cp_sid) == programs_[ib_sid].end()) {

        // try to compile the shader for this combo of image buffer unpack
        // and colour pipeline components

        try {

            programs_[ib_sid][cp_sid].reset(new GLShaderProgram(
                default_vertex_shader,
                colour_pipeline_shader->shader_code_,
                image_buffer_unpack_shader->shader_code_));

        } catch (std::exception &e) {
            spdlog::error("{}", e.what());
            programs_[ib_sid][cp_sid].reset();
        }
    }

    if (programs_[ib_sid][cp_sid]) {
        active_shader_program_ = programs_[ib_sid][cp_sid];
    } else {
        active_shader_program_ = no_image_shader_program_;
    }

    return active_shader_program_ != no_image_shader_program_;
}

void OpenGLViewportRenderer::pre_init() {

    glewInit();

    // we need to know if we have alpha in our draw buffer, which might require
    // different strategies for drawing overlays
    int alpha_bits;
    glGetIntegerv(GL_ALPHA_BITS, &alpha_bits);
    has_alpha_ = alpha_bits != 0;

    // N.B. - if sharing of GL contexts is set-up for multiple GL viewport
    // then we only create one set of textures and use them in both viewports
    // thus meaning we only upload image data once.
    static std::vector<GLTexturePtr> shared_textures;

    if (shared_textures.size()) {
        textures_ = shared_textures;
    } else {
        textures_.emplace_back(new GLDoubleBufferedTexture());
        if (gl_context_shared_) {
            shared_textures = textures_;
        }
    }

    glGenBuffers(1, &vbo_);
    glGenVertexArrays(1, &vao_);

    // add shader for no image render
    no_image_shader_program_ =
        GLShaderProgramPtr(static_cast<GLShaderProgram *>(new NoImageShaderProgram()));
}