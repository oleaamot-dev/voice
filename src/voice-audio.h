/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GStreamer recording (Opus in Ogg, with level metering) and playback. */
#ifndef VOICE_AUDIO_H
#define VOICE_AUDIO_H

#include <glib.h>

G_BEGIN_DECLS

/* Called on the main thread for every level sample (peak: 0.0 .. 1.0). */
typedef void (*VoiceLevelFunc) (gdouble peak, guint64 position_ms, gpointer user_data);

/* Called on the main thread when recording/playback ended on its own. */
typedef void (*VoiceDoneFunc)  (gboolean ok, guint64 duration_ms,
                                const gchar *message, gpointer user_data);

typedef struct _VoiceRecorder VoiceRecorder;
typedef struct _VoicePlayer   VoicePlayer;

VoiceRecorder *voice_recorder_new       (VoiceLevelFunc level_cb,
                                         VoiceDoneFunc  done_cb,
                                         gpointer       user_data);
gboolean       voice_recorder_start     (VoiceRecorder *r, const gchar *path, GError **error);
void           voice_recorder_stop      (VoiceRecorder *r);   /* async: done_cb fires after EOS */
gboolean       voice_recorder_is_active (VoiceRecorder *r);
void           voice_recorder_free      (VoiceRecorder *r);

VoicePlayer   *voice_player_new         (VoiceDoneFunc done_cb, gpointer user_data);
gboolean       voice_player_play        (VoicePlayer *p, const gchar *path, GError **error);
void           voice_player_stop        (VoicePlayer *p);
gboolean       voice_player_is_playing  (VoicePlayer *p);
void           voice_player_free        (VoicePlayer *p);

G_END_DECLS

#endif /* VOICE_AUDIO_H */
