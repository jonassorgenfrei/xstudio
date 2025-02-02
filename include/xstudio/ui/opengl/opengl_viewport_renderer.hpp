// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <memory>
#include <vector>

#include "xstudio/colour_pipeline/colour_pipeline.hpp"
#include "xstudio/ui/viewport/viewport.hpp"
#include "xstudio/utility/uuid.hpp"
#include <Imath/ImathMatrix.h>
#include <caf/actor.hpp>

namespace xstudio {
namespace ui {
    namespace opengl {

        class GLDoubleBufferedTexture;
        class GLColourLutTexture;
        class GLShaderProgram;

        typedef std::shared_ptr<GLShaderProgram> GLShaderProgramPtr;

        class ColourPipeLutCollection {
          public:
            ColourPipeLutCollection() = default;
            ColourPipeLutCollection(const ColourPipeLutCollection &o);

            void upload_luts(
                const std::vector<colour_pipeline::ColourLUTPtr> &luts,
                const bool is_main_viewer);

            void bind_luts(GLShaderProgramPtr shader, int &tex_idx);

          private:
            typedef std::shared_ptr<GLColourLutTexture> GLColourLutTexturePtr;
            std::map<std::string, GLColourLutTexturePtr> lut_textures_;
            std::vector<GLColourLutTexturePtr> active_luts_;
        };

        class OpenGLViewportRenderer : public viewport::ViewportRenderer {
          public:
            OpenGLViewportRenderer(const bool is_main_viewer, const bool gl_context_shared);

            ~OpenGLViewportRenderer() override = default;

            void render(
                const std::vector<media_reader::ImageBufPtr> &next_images,
                const Imath::M44f &to_scene_matrix,
                const Imath::M44f &projection_matrix,
                const Imath::M44f &fit_mode_matrix) override;

          private:
            void pre_init() override;

            bool activate_shader(
                const viewport::GPUShaderPtr &image_buffer_unpack_shader,
                const viewport::GPUShaderPtr &colour_pipeline_shader);

            void
            upload_image_and_colour_data(std::vector<media_reader::ImageBufPtr> next_images);
            void bind_textures();
            void release_textures();
            void clear_viewport_area(const Imath::M44f &to_scene_matrix);

            typedef std::shared_ptr<GLDoubleBufferedTexture> GLTexturePtr;
            std::vector<GLTexturePtr> textures_;

            std::map<utility::Uuid, std::map<utility::Uuid, GLShaderProgramPtr>> programs_;

            ColourPipeLutCollection colour_pipe_textures_;

            unsigned int vbo_, vao_;

            GLShaderProgramPtr active_shader_program_;
            GLShaderProgramPtr no_image_shader_program_;
            std::string latest_colour_pipe_data_cacheid_;
            bool gl_context_shared_;

            media_reader::ImageBufPtr onscreen_frame_;

            bool is_main_viewer_;
            bool has_alpha_ = {false};
        };
    } // namespace opengl
} // namespace ui
} // namespace xstudio
