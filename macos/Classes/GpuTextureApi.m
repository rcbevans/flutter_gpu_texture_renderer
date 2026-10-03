#import <Foundation/Foundation.h>
#import "flutter_gpu_texture_renderer/flutter_gpu_texture_renderer-Swift.h"

#if __cplusplus
extern "C" {
#endif

/// Keep the same name with Windows. `output` is the GpuTextureOutput object
/// pointer handed to dart by `output()`; `texture` carries the latest
/// IOSurface id from the decoder (see hwcodec ffmpeg_vram_decode_mac.mm).
void FlutterGpuTextureRendererPluginCApiSetTexture(void* output, void* texture) {
    GpuTextureOutput* gpu_texture = (__bridge GpuTextureOutput *)(output);
    [gpu_texture markFrameAvaliableWithId:(uint32_t)(uintptr_t)texture];
}

#if __cplusplus
} //Extern C
#endif
