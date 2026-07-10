#include "mpv_texture.h"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

// EGLImage extension function pointers
typedef EGLImageKHR (*PFNEGLCREATEIMAGEKHRPROC)(EGLDisplay, EGLContext, EGLenum, EGLClientBuffer, const EGLint*);
typedef EGLBoolean (*PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay, EGLImageKHR);
typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, GLeglImageOES);

static PFNEGLCREATEIMAGEKHRPROC _eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC _eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC _glEGLImageTargetTexture2DOES = nullptr;

static void init_egl_image_extensions() {
  static bool initialized = false;
  if (!initialized) {
    _eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    _eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    _glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    initialized = true;
  }
}

// Double-buffered mpv-side render slots. The player's render thread draws
// into the BACK slot in mpv's isolated EGL context and flips `front`;
// populate() (GTK thread, Flutter's context) only binds the FRONT slot's
// EGLImage. This decouples mpv's render/swap loop from Flutter's frame-clock
// cadence — rendering inside populate() chained every frame to the
// compositor tick and could not sustain the video rate on weak GPUs.
struct _MpvTexture {
  FlTextureGL parent_instance;

  mpv::MpvPlayer* player;         // not owned
  FlTextureRegistrar* registrar;  // not owned
  FlView* view;                   // not owned, for querying allocation size

  // mpv-side slot resources (owned by mpv's isolated EGL context; touched
  // only by the render thread once it is running)
  GLuint mpv_fbo[2];
  GLuint mpv_texture[2];
  EGLImageKHR egl_image[2];
  int32_t slot_width[2];
  int32_t slot_height[2];
  guint64 slot_serial[2];  // bumped when a slot's EGLImage is (re)created

  // Flutter-side textures (owned by Flutter's context; touched only in
  // populate on the GTK thread)
  GLuint flutter_texture[2];
  guint64 flutter_serial[2];  // slot_serial the flutter texture was built from

  // 1x1 black texture returned while the pipeline bootstraps — the engine
  // treats a FALSE populate as a hard error (it dereferences the GError).
  GLuint placeholder_texture;

  gint front;  // g_atomic: slot index Flutter should display
};

G_DEFINE_TYPE(MpvTexture, mpv_texture, fl_texture_gl_get_type())

// Create/resize one slot's FBO + texture + EGLImage. Runs on the render
// thread with mpv's isolated EGL context current.
static gboolean ensure_slot(MpvTexture* self, int slot, int32_t w, int32_t h) {
  if (self->mpv_fbo[slot] != 0 && self->slot_width[slot] == w && self->slot_height[slot] == h) {
    return TRUE;
  }

  EGLDisplay egl_display = self->player->GetEglDisplay();
  EGLContext egl_context = self->player->GetEglContext();

  if (self->mpv_texture[slot] != 0) {
    glDeleteTextures(1, &self->mpv_texture[slot]);
  }
  if (self->mpv_fbo[slot] != 0) {
    glDeleteFramebuffers(1, &self->mpv_fbo[slot]);
  }
  if (self->egl_image[slot] != EGL_NO_IMAGE_KHR) {
    _eglDestroyImageKHR(egl_display, self->egl_image[slot]);
    self->egl_image[slot] = EGL_NO_IMAGE_KHR;
  }

  self->slot_width[slot] = w;
  self->slot_height[slot] = h;

  glGenTextures(1, &self->mpv_texture[slot]);
  glBindTexture(GL_TEXTURE_2D, self->mpv_texture[slot]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

  glGenFramebuffers(1, &self->mpv_fbo[slot]);
  glBindFramebuffer(GL_FRAMEBUFFER, self->mpv_fbo[slot]);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->mpv_texture[slot], 0);

  EGLint image_attribs[] = {EGL_NONE};
  self->egl_image[slot] = _eglCreateImageKHR(egl_display, egl_context, EGL_GL_TEXTURE_2D_KHR,
                                             (EGLClientBuffer)(uintptr_t)self->mpv_texture[slot], image_attribs);

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (self->egl_image[slot] == EGL_NO_IMAGE_KHR) {
    return FALSE;
  }

  self->slot_serial[slot] += 1;
  return TRUE;
}

gint mpv_texture_render_back(MpvTexture* self) {
  if (!self || !self->player) return -1;

  int32_t w = self->player->SurfaceWidth();
  int32_t h = self->player->SurfaceHeight();
  if (w <= 0 || h <= 0) return -1;

  int back = 1 - g_atomic_int_get(&self->front);
  if (!ensure_slot(self, back, w, h)) return -1;

  glBindFramebuffer(GL_FRAMEBUFFER, self->mpv_fbo[back]);
  self->player->ClearRedrawFlag();
  bool drew = self->player->Render(w, h, static_cast<int>(self->mpv_fbo[back]));
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!drew) {
    // Vsync repeat: consumed without drawing — the front slot already shows
    // this frame, so don't flip (the back slot holds an older frame).
    return 0;
  }
  // Make the writes visible to Flutter's context before flipping. glFlush
  // (not glFinish) — a full per-frame GPU drain costs enough on weak GPUs to
  // blow the frame budget, and same-GPU EGLImage coherency only needs the
  // commands flushed (the pre-render-thread code also only flushed).
  glFlush();

  g_atomic_int_set(&self->front, back);
  return 1;
}

static gboolean mpv_texture_populate(
    FlTextureGL* gl_texture, uint32_t* target, uint32_t* name, uint32_t* width, uint32_t* height, GError** error) {
  MpvTexture* self = MPV_TEXTURE(gl_texture);

  if (!self->player) {
    g_set_error(error, g_quark_from_static_string("mpv"), 0, "Texture has no player");
    return FALSE;
  }

  // Lazily create the mpv render context on first populate() call if the
  // plugin didn't already — Flutter's GL context is current here.
  if (!self->player->HasRenderContext()) {
    if (!self->player->InitRenderContext()) {
      g_set_error(error, g_quark_from_static_string("mpv"), 0, "Failed to create mpv render context");
      return FALSE;
    }
  }

  // Report the target size and make sure the render thread is running; it
  // renders into the back slot and flips `front` when a frame is ready.
  GtkAllocation alloc;
  gtk_widget_get_allocation(GTK_WIDGET(self->view), &alloc);
  int scale = gtk_widget_get_scale_factor(GTK_WIDGET(self->view));
  int32_t w = alloc.width * scale;
  int32_t h = alloc.height * scale;

  if (w > 0 && h > 0) {
    self->player->SetSurfaceSize(w, h);
    self->player->StartRenderThread();
  }

  int idx = g_atomic_int_get(&self->front);
  if (w <= 0 || h <= 0 || self->mpv_texture[idx] == 0 || self->egl_image[idx] == EGL_NO_IMAGE_KHR) {
    // No frame rendered yet (pipeline still bootstrapping) or the view has
    // no size — show black instead of failing populate.
    if (self->placeholder_texture == 0) {
      static const uint8_t black[4] = {0, 0, 0, 255};
      glGenTextures(1, &self->placeholder_texture);
      glBindTexture(GL_TEXTURE_2D, self->placeholder_texture);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
      glBindTexture(GL_TEXTURE_2D, 0);
    }
    *target = GL_TEXTURE_2D;
    *name = self->placeholder_texture;
    *width = 1;
    *height = 1;
    return TRUE;
  }

  // (Re)create the Flutter-side texture if the slot's EGLImage changed.
  if (self->flutter_texture[idx] == 0 || self->flutter_serial[idx] != self->slot_serial[idx]) {
    if (self->flutter_texture[idx] != 0) {
      glDeleteTextures(1, &self->flutter_texture[idx]);
    }
    glGenTextures(1, &self->flutter_texture[idx]);
    glBindTexture(GL_TEXTURE_2D, self->flutter_texture[idx]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    _glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, self->egl_image[idx]);
    glBindTexture(GL_TEXTURE_2D, 0);
    self->flutter_serial[idx] = self->slot_serial[idx];
  }

  *target = GL_TEXTURE_2D;
  *name = self->flutter_texture[idx];
  *width = static_cast<uint32_t>(self->slot_width[idx]);
  *height = static_cast<uint32_t>(self->slot_height[idx]);

  return TRUE;
}

static void mpv_texture_class_init(MpvTextureClass* klass) {
  FL_TEXTURE_GL_CLASS(klass)->populate = mpv_texture_populate;
}

static void mpv_texture_init(MpvTexture* self) {
  self->player = nullptr;
  self->registrar = nullptr;
  self->view = nullptr;
  for (int i = 0; i < 2; i++) {
    self->mpv_fbo[i] = 0;
    self->mpv_texture[i] = 0;
    self->egl_image[i] = EGL_NO_IMAGE_KHR;
    self->slot_width[i] = 0;
    self->slot_height[i] = 0;
    self->slot_serial[i] = 0;
    self->flutter_texture[i] = 0;
    self->flutter_serial[i] = 0;
  }
  self->placeholder_texture = 0;
  self->front = 0;
}

MpvTexture* mpv_texture_new(mpv::MpvPlayer* player, FlTextureRegistrar* registrar, FlView* view) {
  init_egl_image_extensions();
  MpvTexture* self = MPV_TEXTURE(g_object_new(MPV_TEXTURE_TYPE, nullptr));
  self->player = player;
  self->registrar = registrar;
  self->view = view;
  return self;
}

void mpv_texture_mark_frame_available(MpvTexture* self) {
  if (self && self->registrar) {
    fl_texture_registrar_mark_texture_frame_available(self->registrar, FL_TEXTURE(self));
  }
}

void mpv_texture_dispose(MpvTexture* self) {
  if (!self) return;

  EGLDisplay egl_display = EGL_NO_DISPLAY;
  EGLContext egl_context = EGL_NO_CONTEXT;

  if (self->player) {
    // The render thread owns the isolated context and writes into our slots;
    // it must be gone before any GL teardown below.
    self->player->StopRenderThread();
    egl_display = self->player->GetEglDisplay();
    egl_context = self->player->GetEglContext();
  }

  // Clean up Flutter's textures (in Flutter's current context)
  for (int i = 0; i < 2; i++) {
    if (self->flutter_texture[i] != 0) {
      glDeleteTextures(1, &self->flutter_texture[i]);
      self->flutter_texture[i] = 0;
    }
  }
  if (self->placeholder_texture != 0) {
    glDeleteTextures(1, &self->placeholder_texture);
    self->placeholder_texture = 0;
  }

  // Clean up EGLImages
  if (egl_display != EGL_NO_DISPLAY) {
    for (int i = 0; i < 2; i++) {
      if (self->egl_image[i] != EGL_NO_IMAGE_KHR) {
        _eglDestroyImageKHR(egl_display, self->egl_image[i]);
        self->egl_image[i] = EGL_NO_IMAGE_KHR;
      }
    }
  }

  // Clean up mpv's GL resources in mpv's context (released by the joined
  // render thread, so it can be made current here)
  if (egl_context != EGL_NO_CONTEXT) {
    EGLDisplay cur_display = eglGetCurrentDisplay();
    EGLContext cur_context = eglGetCurrentContext();
    EGLSurface cur_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface cur_read = eglGetCurrentSurface(EGL_READ);

    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context);

    for (int i = 0; i < 2; i++) {
      if (self->mpv_texture[i] != 0) {
        glDeleteTextures(1, &self->mpv_texture[i]);
        self->mpv_texture[i] = 0;
      }
      if (self->mpv_fbo[i] != 0) {
        glDeleteFramebuffers(1, &self->mpv_fbo[i]);
        self->mpv_fbo[i] = 0;
      }
    }

    eglMakeCurrent(cur_display, cur_draw, cur_read, cur_context);
  }

  self->player = nullptr;
  self->registrar = nullptr;
  self->view = nullptr;
}

int64_t mpv_texture_get_id(MpvTexture* self) { return fl_texture_get_id(FL_TEXTURE(self)); }
