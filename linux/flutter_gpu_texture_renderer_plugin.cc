#include "gpu_texture_gl.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <unordered_map>

#include "include/flutter_gpu_texture_renderer/flutter_gpu_texture_renderer_plugin.h"

#define GPU_TEXTURE_RENDERER_PLUGIN(obj)                                   \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), gpu_texture_renderer_plugin_get_type(), \
                              GpuTextureRendererPlugin))

typedef struct _GpuTextureRendererPlugin GpuTextureRendererPlugin;

struct _GpuTextureRendererPlugin {
  GObject parent_instance;
  FlTextureRegistrar *texture_registrar;
};

G_DEFINE_TYPE(GpuTextureRendererPlugin, gpu_texture_renderer_plugin,
              g_object_get_type())
static std::unordered_map<int64_t, GpuTextureGL *> g_output_map;

static void gpu_texture_renderer_plugin_handle_method_call(
    GpuTextureRendererPlugin *self,
    FlMethodCall *method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;

  const gchar *method = fl_method_call_get_name(method_call);
  auto args = fl_method_call_get_args(method_call);

  if (strcmp(method, "registerTexture") == 0) {
    GpuTextureGL *texture = gpu_texture_gl_new(self->texture_registrar);
    if (texture == nullptr) {
      response = FL_METHOD_RESPONSE(
          fl_method_error_response_new("registerTexture",
                                       "Failed to register texture.", nullptr));
    } else {
      int64_t id = reinterpret_cast<int64_t>(FL_TEXTURE(texture));
      g_output_map[id] = texture;
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(
          fl_value_new_int(id)));
    }
  } else if (strcmp(method, "unregisterTexture") == 0) {
    int64_t id = fl_value_get_int(fl_value_lookup_string(args, "id"));
    auto it = g_output_map.find(id);
    if (it != g_output_map.end()) {
      fl_texture_registrar_unregister_texture(self->texture_registrar,
                                              FL_TEXTURE(it->second));
      g_object_unref(it->second);
      g_output_map.erase(it);
    }
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (strcmp(method, "output") == 0) {
    int64_t id = fl_value_get_int(fl_value_lookup_string(args, "id"));
    auto it = g_output_map.find(id);
    if (it != g_output_map.end()) {
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(
          fl_value_new_int(reinterpret_cast<int64_t>(it->second))));
    } else {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "output", "Output not found.", nullptr));
    }
  } else if (strcmp(method, "fps") == 0) {
    int64_t id = fl_value_get_int(fl_value_lookup_string(args, "id"));
    auto it = g_output_map.find(id);
    if (it != g_output_map.end()) {
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(
          fl_value_new_int((int16_t)gpu_texture_gl_take_fps(it->second))));
    } else {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new(
          "fps", "Output not found.", nullptr));
    }
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  fl_method_call_respond(method_call, response, nullptr);
}

static void gpu_texture_renderer_plugin_dispose(GObject *object) {
  GpuTextureRendererPlugin *self = GPU_TEXTURE_RENDERER_PLUGIN(object);
  for (auto &entry : g_output_map) {
    fl_texture_registrar_unregister_texture(self->texture_registrar,
                                            FL_TEXTURE(entry.second));
    g_object_unref(entry.second);
  }
  g_output_map.clear();

  G_OBJECT_CLASS(gpu_texture_renderer_plugin_parent_class)->dispose(object);
}

static void gpu_texture_renderer_plugin_class_init(
    GpuTextureRendererPluginClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = gpu_texture_renderer_plugin_dispose;
}

static void gpu_texture_renderer_plugin_init(GpuTextureRendererPlugin *self) {
  (void)self;
}

static void method_call_cb(FlMethodChannel *channel,
                           FlMethodCall *method_call,
                           gpointer user_data) {
  (void)channel;
  GpuTextureRendererPlugin *plugin =
      GPU_TEXTURE_RENDERER_PLUGIN(user_data);
  gpu_texture_renderer_plugin_handle_method_call(plugin, method_call);
}

void flutter_gpu_texture_renderer_plugin_register_with_registrar(
    FlPluginRegistrar *registrar) {
  GpuTextureRendererPlugin *plugin = GPU_TEXTURE_RENDERER_PLUGIN(
      g_object_new(gpu_texture_renderer_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "flutter_gpu_texture_renderer", FL_METHOD_CODEC(codec));
  plugin->texture_registrar =
      fl_plugin_registrar_get_texture_registrar(registrar);
  fl_method_channel_set_method_call_handler(channel, method_call_cb,
                                            g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}

extern "C" {
void FlutterGpuTextureRendererPluginCApiSetNv12(void *output,
                                                const void *y,
                                                const void *uv,
                                                int y_stride,
                                                int uv_stride,
                                                int width,
                                                int height) {
  if (output == nullptr) {
    return;
  }
  gpu_texture_gl_post_nv12(GPU_TEXTURE_GL(output), y, uv, y_stride, uv_stride,
                           width, height);
}
}  // extern "C"
