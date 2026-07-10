#ifndef MPV_PLAYER_H_
#define MPV_PLAYER_H_

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

// Forward declaration for Flutter types
struct _FlValue;

namespace mpv {

/// Callback function type for mpv events.
/// Note: FlValue* is passed from the global namespace, not mpv namespace.
using EventCallback = std::function<void(::_FlValue*)>;

/// Callback for requesting a redraw (called from mpv render update thread).
using RedrawCallback = std::function<void()>;

/// Renders the pending mpv frame into the texture's back buffer.
/// Runs on the player's render thread with the isolated EGL context current.
/// Returns: <0 no render target yet (caller consumes the frame cheaply),
///           0 frame consumed without new content (vsync repeat — no redraw),
///          >0 new frame flipped to front (caller marks the texture).
using RenderCallback = std::function<int()>;

/// Wrapper for libmpv that handles initialization, OpenGL rendering,
/// commands, properties, and event dispatching.
class MpvPlayer {
 public:
  /// |audio_only| runs mpv as a music core with video disabled entirely:
  /// no render context is ever created (InitRenderContext must not be
  /// called) and no GL/EGL state is touched.
  explicit MpvPlayer(bool audio_only = false);
  ~MpvPlayer();

  /// Initializes the mpv instance and configures options.
  /// Does NOT create the render context — call InitRenderContext() later
  /// when an OpenGL context is available.
  /// @return true if initialization succeeded.
  bool Initialize();

  /// Creates the mpv OpenGL render context.
  /// Must be called with a valid GL context current (e.g., from FlTextureGL::populate).
  /// Fails on audio-only players.
  /// @return true if render context creation succeeded.
  bool InitRenderContext();

  /// Returns true if the render context has been created.
  bool HasRenderContext() const { return mpv_gl_ != nullptr; }

  /// Returns the isolated EGL display used for mpv rendering.
  EGLDisplay GetEglDisplay() const { return egl_display_; }

  /// Returns the isolated EGL context used for mpv rendering.
  EGLContext GetEglContext() const { return egl_context_; }

  /// Disposes mpv and releases resources.
  void Dispose();

  /// Returns true if mpv is initialized (has both mpv handle and render
  /// context; audio-only players never have a render context).
  bool IsInitialized() const { return mpv_ != nullptr && (audio_only_ || mpv_gl_ != nullptr); }

  /// Returns true if this player has been disposed.
  bool IsDisposed() const { return disposed_.load(); }

  /// Returns true if mpv handle exists (even without render context).
  bool HasMpvHandle() const { return mpv_ != nullptr; }

  /// Queues an mpv command without waiting for completion.
  void Command(const std::vector<std::string>& args);

  /// Callback types for async mpv requests.
  using StatusCallback = std::function<void(int error)>;
  using CommandCallback = StatusCallback;
  using GetPropertyCallback = std::function<void(int error, const std::string& value)>;

  /// Executes an mpv command asynchronously to prevent UI blocking.
  void CommandAsync(const std::vector<std::string>& args, CommandCallback callback);

  /// Sets an mpv property by name.
  void SetProperty(const std::string& name, const std::string& value);

  /// Sets an mpv property asynchronously.
  void SetPropertyAsync(const std::string& name, const std::string& value, StatusCallback callback);

  /// Gets an mpv property value asynchronously.
  void GetPropertyAsync(const std::string& name, GetPropertyCallback callback);

  /// Observes an mpv property for changes.
  void ObserveProperty(const std::string& name, const std::string& format, int id);

  /// Renders a frame to the specified FBO.
  /// Returns true if pixels were drawn into |fbo|; false if the pending frame
  /// was a pure vsync repeat and was consumed without drawing.
  bool Render(int width, int height, int fbo = 0);

  /// Reports a display swap to mpv (vsync feedback for display-resample).
  void ReportSwap();

  /// Sets the callback the render thread uses to draw into the texture.
  void SetRenderCallback(RenderCallback callback);

  /// Starts the dedicated render thread (idempotent). Requires the render
  /// context to exist. The thread owns the isolated EGL context from then on.
  void StartRenderThread();

  /// Stops and joins the render thread (idempotent). Must be called before
  /// the texture's GL resources are destroyed.
  void StopRenderThread();

  /// Latest video surface size, reported by populate() on the GTK thread and
  /// consumed by the render thread.
  void SetSurfaceSize(int width, int height) {
    surface_width_.store(width);
    surface_height_.store(height);
  }
  int SurfaceWidth() const { return surface_width_.load(); }
  int SurfaceHeight() const { return surface_height_.load(); }

  /// Reports that the mouse has moved.
  void ReportMouseMove(int x, int y);

  /// Sets the event callback for property changes and events.
  void SetEventCallback(EventCallback callback);

  /// Sets the redraw callback (called when mpv has a new frame ready).
  void SetRedrawCallback(RedrawCallback callback);

  /// Returns true if a redraw is needed.
  bool NeedsRedraw() const { return needs_redraw_.load(); }

  /// Clears the redraw flag.
  void ClearRedrawFlag() { needs_redraw_.store(false); }

  /// Sets the MPV log message level (e.g., "warn", "v", "debug").
  void SetLogLevel(const std::string& level);

 private:
  /// MPV event wakeup callback (called from mpv thread).
  static void OnMpvWakeup(void* ctx);

  /// MPV render update callback (called when frame is ready).
  static void OnMpvRenderUpdate(void* ctx);

  /// Processes pending mpv events.
  bool ProcessEvents();

  /// Handles a single mpv event.
  void HandleMpvEvent(mpv_event* event);

  /// Sends a property change notification.
  void SendPropertyChange(const char* name, mpv_node* data);

  /// Sends an event notification.
  void SendEvent(const std::string& name, ::_FlValue* data = nullptr);

  uint64_t RegisterStatusRequest(StatusCallback callback);
  StatusCallback TakeStatusRequest(uint64_t request_id);
  uint64_t RegisterGetPropertyRequest(GetPropertyCallback callback);
  GetPropertyCallback TakeGetPropertyRequest(uint64_t request_id);

  /// Helper to convert mpv_node to FlValue.
  ::_FlValue* NodeToFlValue(mpv_node* node);

  const bool audio_only_;
  mpv_handle* mpv_ = nullptr;
  mpv_render_context* mpv_gl_ = nullptr;

  // Isolated EGL context for mpv rendering (not shared with Flutter)
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLContext egl_context_ = EGL_NO_CONTEXT;

  std::atomic<bool> needs_redraw_{false};
  std::atomic<bool> disposed_{false};

  // Last size actually rendered by mpv; a mismatch means the FBO was recreated
  // and the vsync-repeat fast path in Render() must not skip the render.
  int last_render_width_ = 0;
  int last_render_height_ = 0;

  // TEST INSTRUMENTATION (drop before upstream PR): pacing heartbeat source.
  guint metrics_timer_id_ = 0;
  EventCallback event_callback_;
  RedrawCallback redraw_callback_;
  RenderCallback render_callback_;
  std::mutex callback_mutex_;

  // Dedicated render thread: decouples mpv's render/swap loop from Flutter's
  // frame-clock ticks (rendering inside populate() chained every frame to the
  // compositor cadence and could not sustain the video rate on weak GPUs).
  std::thread render_thread_;
  std::mutex render_thread_mutex_;
  std::condition_variable render_cv_;
  bool render_pending_ = false;
  std::atomic<bool> render_thread_running_{false};
  std::atomic<bool> render_thread_stop_{false};
  std::atomic<int> surface_width_{0};
  std::atomic<int> surface_height_{0};

  uint64_t next_reply_userdata_ = 1;
  std::map<std::string, uint64_t> observed_properties_;
  std::map<std::string, int> name_to_id_;

  // Pending async requests: request_id -> callback
  std::map<uint64_t, StatusCallback> pending_status_requests_;
  std::map<uint64_t, GetPropertyCallback> pending_get_property_requests_;
  std::mutex pending_requests_mutex_;

  // GSource for processing events on main thread
  guint event_source_id_ = 0;
};

}  // namespace mpv

#endif  // MPV_PLAYER_H_
