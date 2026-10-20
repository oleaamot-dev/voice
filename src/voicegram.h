/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Voicegram: metadata + level history of one recorded voice message,
 * stored as XML next to the audio file in ~/GNOME.voice. */
#ifndef VOICEGRAM_H
#define VOICEGRAM_H

#include <glib.h>

G_BEGIN_DECLS

#define VOICEGRAM_FORMAT_VERSION      "51.0"
#define VOICEGRAM_DEFAULT_INTERVAL_MS 100
#define VOICEGRAM_MAX_LEVELS          200000

typedef struct {
  gchar   *id;          /* [A-Za-z0-9_-]{1,64}, also the file base name  */
  gchar   *title;
  gchar   *created;     /* ISO 8601, UTC                                  */
  gchar   *audio_file;  /* base name inside the GNOME.voice directory     */
  gchar   *codec;       /* e.g. audio/x-opus                              */
  gchar   *remote_id;   /* id assigned by the server after upload, or NULL */
  guint64  duration_ms;
  guint64  size_bytes;
  guint    interval_ms; /* time between two level samples                 */
  GArray  *levels;      /* gdouble, linear peak level 0.0 .. 1.0          */
} Voicegram;

Voicegram *voicegram_new      (void);
Voicegram *voicegram_new_now  (void);
void       voicegram_free     (Voicegram *vg);

gboolean   voicegram_valid_id (const gchar *id);

gchar     *voicegram_to_xml   (const Voicegram *vg);
Voicegram *voicegram_from_xml (const gchar *xml, gssize len, GError **error);

gchar     *voicegram_xml_path (const gchar *dir, const gchar *id);
gboolean   voicegram_save     (const Voicegram *vg, const gchar *dir, GError **error);
Voicegram *voicegram_load     (const gchar *path, GError **error);

/* All voicegrams found in dir, newest first. Element type: Voicegram*. */
GPtrArray *voicegram_list     (const gchar *dir);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Voicegram, voicegram_free)

G_END_DECLS

#endif /* VOICEGRAM_H */
