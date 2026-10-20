/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HTTPS client (OpenSSL, TLS 1.2+, certificate and hostname verification
 * always on) for uploading and downloading voice messages. */
#ifndef VOICE_NET_H
#define VOICE_NET_H

#include <glib.h>

G_BEGIN_DECLS

#define VOICE_NET_ERROR (voice_net_error_quark ())

typedef enum {
  VOICE_NET_ERROR_INVALID,   /* bad argument / unsafe id          */
  VOICE_NET_ERROR_CONNECT,   /* DNS / TCP                         */
  VOICE_NET_ERROR_TLS,       /* handshake or certificate failure  */
  VOICE_NET_ERROR_IO,        /* read/write/timeout                */
  VOICE_NET_ERROR_PROTOCOL,  /* malformed HTTP or unexpected body */
  VOICE_NET_ERROR_HTTP       /* non-2xx status                    */
} VoiceNetError;

GQuark       voice_net_error_quark   (void);

/* GNOME_VOICE_HOST / GNOME_VOICE_PORT override the compiled-in defaults. */
const gchar *voice_net_default_host  (void);
const gchar *voice_net_default_port  (void);

/* POST the Voicegram XML and the audio file. On success *remote_id receives
 * the identifier assigned by the server. */
gboolean voice_net_upload   (const gchar *host, const gchar *port,
                             const gchar *xml_path, const gchar *audio_path,
                             gchar **remote_id, GError **error);

/* GET the Voicegram XML and audio for remote_id and store both in dest_dir
 * as <remote_id>.xml and <remote_id>.ogg. */
gboolean voice_net_download (const gchar *host, const gchar *port,
                             const gchar *remote_id, const gchar *dest_dir,
                             GError **error);

G_END_DECLS

#endif /* VOICE_NET_H */
