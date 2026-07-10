#ifndef MPV_TEXTURE_H_
#define MPV_TEXTURE_H_

#include <flutter_linux/flutter_linux.h>

#include "mpv_player.h"

G_BEGIN_DECLS

#define MPV_TEXTURE_TYPE (mpv_texture_get_type())

G_DECLARE_FINAL_TYPE(MpvTexture, mpv_texture, MPV, TEXTURE, FlTextureGL)

/// Creates a new MpvTexture that renders mpv video to an offscreen FBO.
MpvTexture* mpv_texture_new(mpv::MpvPlayer* player, FlTextureRegistrar* registrar, FlView* view);

/// Notifies Flutter that a new frame is available.
void mpv_texture_mark_frame_available(MpvTexture* self);

/// Renders the pending mpv frame into the back slot and flips it to front.
/// Must run on the player's render thread with the isolated EGL context
/// current. Returns <0 if there is no usable render target yet, 0 if the
/// frame was a vsync repeat (consumed, nothing new to show), >0 if a new
/// frame was flipped to front.
gint mpv_texture_render_back(MpvTexture* self);

/// Cleans up GL resources (FBO/texture).
void mpv_texture_dispose(MpvTexture* self);

/// Returns the Flutter texture ID.
int64_t mpv_texture_get_id(MpvTexture* self);

G_END_DECLS

#endif  // MPV_TEXTURE_H_
