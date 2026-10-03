#ifndef FLUTTER_PLUGIN_GPU_TEXTURE_GL_H_
#define FLUTTER_PLUGIN_GPU_TEXTURE_GL_H_

#include <flutter_linux/flutter_linux.h>

G_BEGIN_DECLS

G_DECLARE_FINAL_TYPE(GpuTextureGL, gpu_texture_gl, GPU, TEXTURE_GL,
                     FlTextureGL)

GpuTextureGL *gpu_texture_gl_new(FlTextureRegistrar *registrar);

void gpu_texture_gl_post_nv12(GpuTextureGL *self,
                              const void *y,
                              const void *uv,
                              int y_stride,
                              int uv_stride,
                              int width,
                              int height);

uint32_t gpu_texture_gl_take_fps(GpuTextureGL *self);

G_END_DECLS

#endif  // FLUTTER_PLUGIN_GPU_TEXTURE_GL_H_
