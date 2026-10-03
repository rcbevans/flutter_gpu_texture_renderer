#include "gpu_texture_gl.h"

#include <epoxy/gl.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace {
// GL objects must be deleted on a thread with the context current, but the
// GObject finalizer runs on the main thread without one. Finalized textures
// queue their names here; the next populate() on any live texture drains the
// list in the (shared) raster context.
std::mutex g_orphan_mutex;
std::vector<GLuint> g_orphan_textures;
std::vector<GLuint> g_orphan_framebuffers;
std::vector<GLuint> g_orphan_programs;

// Guards both the live set and, while held for a post, the whole post
// (live-check + memcpy) so it cannot interleave with dispose freeing the
// pending buffer. Populate only takes the per-texture mutex.
std::mutex g_live_mutex;
std::vector<GpuTextureGL *> g_live;

// Sanity bounds; also keep stride arithmetic inside size_t without surprises.
constexpr int kMaxFrameDim = 16384;
constexpr int kMaxStride = 65536;

GQuark gpu_texture_gl_error_quark() {
  static GQuark quark =
      g_quark_from_static_string("flutter_gpu_texture_renderer");
  return quark;
}

GLuint compile_shader(GLenum type, const char *source, GError **error) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[512] = {};
    glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
    glDeleteShader(shader);
    g_set_error(error, gpu_texture_gl_error_quark(), 1,
                "shader compile failed: %s", log);
    return 0;
  }
  return shader;
}

GLuint link_program(GError **error) {
  // Fullscreen triangle from gl_VertexID: no buffers, valid in core and
  // compatibility profiles. Covers NDC (-1,-1)..(1,1) with v_uv 0..1 over the
  // viewport (uv beyond 1 lies outside the triangle).
  const char *vs =
      "#version 130\n"
      "out vec2 v_uv;\n"
      "void main() {\n"
      "  vec2 pos = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
      "  gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);\n"
      "  v_uv = min(pos, 1.0);\n"
      "}\n";
  // BT.601 limited-range, matching the libyuv conversion of the software
  // path; .r = Y / Cb, .g = Cr.
  const char *fs =
      "#version 130\n"
      "in vec2 v_uv;\n"
      "uniform sampler2D y_tex;\n"
      "uniform sampler2D uv_tex;\n"
      "out vec4 frag_color;\n"
      "void main() {\n"
      "  float y = 1.164 * (texture(y_tex, v_uv).r - 0.0625);\n"
      "  vec2 uv = texture(uv_tex, v_uv).rg - 0.5;\n"
      "  frag_color = vec4(\n"
      "    clamp(y + 1.596 * uv.g, 0.0, 1.0),\n"
      "    clamp(y - 0.392 * uv.r - 0.813 * uv.g, 0.0, 1.0),\n"
      "    clamp(y + 2.017 * uv.r, 0.0, 1.0), 1.0);\n"
      "}\n";

  GLuint vs_id = compile_shader(GL_VERTEX_SHADER, vs, error);
  if (vs_id == 0) {
    return 0;
  }
  GLuint fs_id = compile_shader(GL_FRAGMENT_SHADER, fs, error);
  if (fs_id == 0) {
    glDeleteShader(vs_id);
    return 0;
  }
  GLuint program = glCreateProgram();
  glAttachShader(program, vs_id);
  glAttachShader(program, fs_id);
  glLinkProgram(program);
  glDeleteShader(vs_id);
  glDeleteShader(fs_id);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[512] = {};
    glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    glDeleteProgram(program);
    g_set_error(error, gpu_texture_gl_error_quark(), 2,
                "program link failed: %s", log);
    return 0;
  }
  // Samplers are fixed to the units used by the draw.
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "y_tex"), 0);
  glUniform1i(glGetUniformLocation(program, "uv_tex"), 1);
  glUseProgram(0);
  return program;
}

// The engine may populate before the first frame arrives; an incomplete or
// 0-sized texture must never be handed to Skia.
constexpr int kFallbackSize = 1;
}  // namespace

struct _GpuTextureGL {
  FlTextureGL parent_instance;

  GMutex mutex;
  // Pending frame, produced by post_nv12 (any thread), consumed by populate
  // (raster thread). One contiguous block: Y plane, then UV plane.
  uint8_t *pending;
  size_t pending_capacity;
  int y_stride;
  int uv_stride;
  int frame_width;
  int frame_height;
  gboolean pending_ready;

  // GL objects, created on first populate; raster thread only.
  gboolean gl_ready;
  GLuint y_tex;
  GLuint uv_tex;
  GLuint fbo_tex;
  GLuint fbo;
  GLuint program;

  // Allocated Y/UV texture size; re-specified on change (0,0 until the
  // first frame). fbo_tex starts at 1x1 (defined-black).
  int tex_width;
  int tex_height;

  FlTextureRegistrar *registrar;
  gint32 posted_since_fps;
};

G_DEFINE_TYPE(GpuTextureGL, gpu_texture_gl, fl_texture_gl_get_type())

static void gpu_texture_gl_drain_orphans() {
  std::lock_guard<std::mutex> lock(g_orphan_mutex);
  if (!g_orphan_textures.empty()) {
    glDeleteTextures(g_orphan_textures.size(), g_orphan_textures.data());
    g_orphan_textures.clear();
  }
  if (!g_orphan_framebuffers.empty()) {
    glDeleteFramebuffers(g_orphan_framebuffers.size(),
                         g_orphan_framebuffers.data());
    g_orphan_framebuffers.clear();
  }
  if (!g_orphan_programs.empty()) {
    for (GLuint program : g_orphan_programs) {
      glDeleteProgram(program);
    }
    g_orphan_programs.clear();
  }
}

static void gpu_texture_gl_ensure_gl(GpuTextureGL *self, GError **error) {
  if (self->gl_ready) {
    return;
  }
  glGenTextures(1, &self->y_tex);
  glGenTextures(1, &self->uv_tex);
  glGenTextures(1, &self->fbo_tex);
  glGenFramebuffers(1, &self->fbo);
  self->program = link_program(error);
  if (self->program == 0) {
    // Free the generated names so a later populate can retry cleanly.
    glDeleteTextures(1, &self->y_tex);
    glDeleteTextures(1, &self->uv_tex);
    glDeleteTextures(1, &self->fbo_tex);
    glDeleteFramebuffers(1, &self->fbo);
    self->y_tex = 0;
    self->uv_tex = 0;
    self->fbo_tex = 0;
    self->fbo = 0;
    return;
  }
  for (GLuint tex : {self->y_tex, self->uv_tex, self->fbo_tex}) {
    glBindTexture(GL_TEXTURE_2D, tex);
    // Nearest: the fbo texture is scaled by Skia; chroma sampling must not
    // smear before the conversion. No mip levels are generated.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, self->fbo_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kFallbackSize, kFallbackSize, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         self->fbo_tex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  self->tex_width = 0;
  self->tex_height = 0;
  self->gl_ready = TRUE;
}

// Uploads the pending frame and converts it in-shader into fbo_tex. Saves and
// restores all GL state it touches (per-unit bindings included); populate is
// called mid-frame with Skia's render state current.
static void gpu_texture_gl_upload_and_convert(GpuTextureGL *self) {
  int uv_width = (self->frame_width + 1) / 2;
  int uv_height = (self->frame_height + 1) / 2;
  const uint8_t *uv_data =
      self->pending + (size_t)self->y_stride * self->frame_height;

  GLint prev_fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
  GLint prev_viewport[4] = {};
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  GLint prev_active = GL_TEXTURE0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  GLint prev_row_length = 0;
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prev_row_length);
  GLint prev_alignment = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
  GLint prev_program = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
  GLint prev_unit0_tex = 0;
  GLint prev_unit1_tex = 0;
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_unit0_tex);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_unit1_tex);
  const GLenum prev_enable_bits[] = {GL_BLEND, GL_SCISSOR_TEST, GL_DEPTH_TEST,
                                     GL_STENCIL_TEST, GL_CULL_FACE};
  GLboolean prev_bits[5];
  for (size_t i = 0; i < 5; i++) {
    prev_bits[i] = glIsEnabled(prev_enable_bits[i]);
  }

  gboolean resize = self->frame_width != self->tex_width ||
                    self->frame_height != self->tex_height;

  // GL_UNPACK_ROW_LENGTH is in pixels: GL_R8 is 1 byte/texel, GL_RG8 is 2.
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, self->y_tex);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, self->y_stride);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (resize) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, self->frame_width,
                 self->frame_height, 0, GL_RED, GL_UNSIGNED_BYTE,
                 self->pending);
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, self->frame_width,
                    self->frame_height, GL_RED, GL_UNSIGNED_BYTE,
                    self->pending);
  }
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, self->uv_tex);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, self->uv_stride / 2);
  if (resize) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uv_width, uv_height, 0, GL_RG,
                 GL_UNSIGNED_BYTE, uv_data);
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_width, uv_height, GL_RG,
                    GL_UNSIGNED_BYTE, uv_data);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, prev_row_length);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);

  if (resize) {
    self->tex_width = self->frame_width;
    self->tex_height = self->frame_height;
    glBindTexture(GL_TEXTURE_2D, self->fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, self->frame_width,
                 self->frame_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  }

  glUseProgram(self->program);
  glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
  glViewport(0, 0, self->frame_width, self->frame_height);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, self->y_tex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, self->uv_tex);
  for (GLenum bit : prev_enable_bits) {
    glDisable(bit);
  }
  glDrawArrays(GL_TRIANGLES, 0, 3);
  for (size_t i = 0; i < 5; i++) {
    if (prev_bits[i]) {
      glEnable(prev_enable_bits[i]);
    }
  }

  // Restore engine state, per-unit bindings included (Skia caches them).
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, prev_unit1_tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, prev_unit0_tex);
  glActiveTexture(prev_active);
  glBindFramebuffer(GL_FRAMEBUFFER, prev_fbo);
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
             prev_viewport[3]);
  glUseProgram(prev_program);
}

static gboolean gpu_texture_gl_populate(FlTextureGL *texture,
                                        uint32_t *target,
                                        uint32_t *name,
                                        uint32_t *width,
                                        uint32_t *height,
                                        GError **error) {
  GpuTextureGL *self = GPU_TEXTURE_GL(texture);
  gpu_texture_gl_drain_orphans();
  gpu_texture_gl_ensure_gl(self, error);
  if (!self->gl_ready) {
    return FALSE;
  }

  g_mutex_lock(&self->mutex);
  if (self->pending_ready) {
    gpu_texture_gl_upload_and_convert(self);
    self->pending_ready = FALSE;
  }
  *target = GL_TEXTURE_2D;
  *name = self->fbo_tex;
  *width = self->frame_width > 0 ? (uint32_t)self->frame_width
                                 : (uint32_t)kFallbackSize;
  *height = self->frame_height > 0 ? (uint32_t)self->frame_height
                                   : (uint32_t)kFallbackSize;
  g_mutex_unlock(&self->mutex);
  return TRUE;
}

GpuTextureGL *gpu_texture_gl_new(FlTextureRegistrar *registrar) {
  GpuTextureGL *self =
      GPU_TEXTURE_GL(g_object_new(gpu_texture_gl_get_type(), nullptr));
  if (!fl_texture_registrar_register_texture(registrar, FL_TEXTURE(self))) {
    g_object_unref(self);
    return nullptr;
  }
  self->registrar = registrar;
  return self;
}

void gpu_texture_gl_post_nv12(GpuTextureGL *self,
                              const void *y,
                              const void *uv,
                              int y_stride,
                              int uv_stride,
                              int width,
                              int height) {
  if (width <= 0 || height <= 0 || y_stride <= 0 || uv_stride <= 0 ||
      width > kMaxFrameDim || height > kMaxFrameDim ||
      y_stride > kMaxStride || uv_stride > kMaxStride) {
    return;
  }
  size_t y_size = (size_t)y_stride * height;
  size_t uv_size = (size_t)uv_stride * ((height + 1) / 2);
  size_t need = y_size + uv_size;

  // Held for the whole post: the live check and the copy are atomic against
  // dispose (which frees the pending buffer under the same lock).
  std::lock_guard<std::mutex> lock(g_live_mutex);
  if (std::find(g_live.begin(), g_live.end(), self) == g_live.end()) {
    return;  // output was unregistered; drop the frame
  }

  g_mutex_lock(&self->mutex);
  if (self->pending_capacity < need) {
    uint8_t *grown = (uint8_t *)g_realloc(self->pending, need);
    if (grown == nullptr) {
      g_mutex_unlock(&self->mutex);
      return;
    }
    self->pending = grown;
    self->pending_capacity = need;
  }
  memcpy(self->pending, y, y_size);
  memcpy(self->pending + y_size, uv, uv_size);
  self->y_stride = y_stride;
  self->uv_stride = uv_stride;
  self->frame_width = width;
  self->frame_height = height;
  self->pending_ready = TRUE;
  g_mutex_unlock(&self->mutex);

  g_atomic_int_add(&self->posted_since_fps, 1);
  fl_texture_registrar_mark_texture_frame_available(self->registrar,
                                                    FL_TEXTURE(self));
}

uint32_t gpu_texture_gl_take_fps(GpuTextureGL *self) {
  return g_atomic_int_exchange(&self->posted_since_fps, 0);
}

static void gpu_texture_gl_dispose(GObject *object) {
  GpuTextureGL *self = GPU_TEXTURE_GL(object);
  // Serializes against in-flight posts (g_live_mutex) and against populate
  // (self->mutex) before the pending buffer and the GL names go away.
  std::lock_guard<std::mutex> live_lock(g_live_mutex);
  g_live.erase(std::remove(g_live.begin(), g_live.end(), self), g_live.end());
  g_mutex_lock(&self->mutex);
  {
    std::lock_guard<std::mutex> lock(g_orphan_mutex);
    if (self->y_tex != 0) {
      g_orphan_textures.push_back(self->y_tex);
    }
    if (self->uv_tex != 0) {
      g_orphan_textures.push_back(self->uv_tex);
    }
    if (self->fbo_tex != 0) {
      g_orphan_textures.push_back(self->fbo_tex);
    }
    if (self->fbo != 0) {
      g_orphan_framebuffers.push_back(self->fbo);
    }
    if (self->program != 0) {
      g_orphan_programs.push_back(self->program);
    }
  }
  self->y_tex = 0;
  self->uv_tex = 0;
  self->fbo_tex = 0;
  self->fbo = 0;
  self->program = 0;
  g_clear_pointer(&self->pending, g_free);
  self->pending_capacity = 0;
  g_mutex_unlock(&self->mutex);

  G_OBJECT_CLASS(gpu_texture_gl_parent_class)->dispose(object);
}

static void gpu_texture_gl_finalize(GObject *object) {
  GpuTextureGL *self = GPU_TEXTURE_GL(object);
  g_mutex_clear(&self->mutex);
  G_OBJECT_CLASS(gpu_texture_gl_parent_class)->finalize(object);
}

static void gpu_texture_gl_class_init(GpuTextureGLClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = gpu_texture_gl_dispose;
  G_OBJECT_CLASS(klass)->finalize = gpu_texture_gl_finalize;
  FL_TEXTURE_GL_CLASS(klass)->populate = gpu_texture_gl_populate;
}

static void gpu_texture_gl_init(GpuTextureGL *self) {
  g_mutex_init(&self->mutex);
  std::lock_guard<std::mutex> lock(g_live_mutex);
  g_live.push_back(self);
}
