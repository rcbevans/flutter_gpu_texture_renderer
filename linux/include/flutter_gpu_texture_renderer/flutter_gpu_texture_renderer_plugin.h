#ifndef FLUTTER_PLUGIN_FLUTTER_GPU_TEXTURE_RENDERER_PLUGIN_H_
#define FLUTTER_PLUGIN_FLUTTER_GPU_TEXTURE_RENDERER_PLUGIN_H_

#include <flutter_linux/flutter_linux.h>

#ifndef FLUTTER_PLUGIN_EXPORT
#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define FLUTTER_PLUGIN_EXPORT
#endif
#endif

G_BEGIN_DECLS

typedef struct _GpuTextureRendererPlugin GpuTextureRendererPlugin;
typedef struct {
  GObjectClass parent_class;
} GpuTextureRendererPluginClass;

FLUTTER_PLUGIN_EXPORT GType gpu_texture_renderer_plugin_get_type();

FLUTTER_PLUGIN_EXPORT void flutter_gpu_texture_renderer_plugin_register_with_registrar(
    FlPluginRegistrar *registrar);

// Posts one NV12 frame (Y plane, then interleaved UV plane) to the output
// obtained via the `output` method call. The Y plane is `height` rows of
// `y_stride` bytes; the UV plane is `(height + 1) / 2` rows of `uv_stride`
// bytes. The frame is copied; the caller may free the buffers on return.
// Safe from any thread. Posts to a disposed output are dropped.
FLUTTER_PLUGIN_EXPORT void FlutterGpuTextureRendererPluginCApiSetNv12(
    void *output,
    const void *y,
    const void *uv,
    int y_stride,
    int uv_stride,
    int width,
    int height);

G_END_DECLS

#endif  // FLUTTER_PLUGIN_FLUTTER_GPU_TEXTURE_RENDERER_PLUGIN_H_
