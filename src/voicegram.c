/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "voicegram.h"

#include <math.h>
#include <string.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

/* ------------------------------------------------------------------ */
/* Construction                                                        */
/* ------------------------------------------------------------------ */

Voicegram *
voicegram_new (void)
{
  Voicegram *vg = g_new0 (Voicegram, 1);
  vg->interval_ms = VOICEGRAM_DEFAULT_INTERVAL_MS;
  vg->levels = g_array_new (FALSE, FALSE, sizeof (gdouble));
  return vg;
}

Voicegram *
voicegram_new_now (void)
{
  Voicegram *vg = voicegram_new ();
  GDateTime *local = g_date_time_new_now_local ();
  GDateTime *utc = g_date_time_to_utc (local);

  gchar *stamp = g_date_time_format (local, "%Y%m%dT%H%M%S");
  vg->id = g_strdup_printf ("%s-%08x", stamp, g_random_int ());
  vg->created = g_date_time_format_iso8601 (utc);
  vg->title = g_date_time_format (local, "Voice message %Y-%m-%d %H:%M");
  vg->codec = g_strdup ("audio/x-opus");

  g_free (stamp);
  g_date_time_unref (utc);
  g_date_time_unref (local);
  return vg;
}

void
voicegram_free (Voicegram *vg)
{
  if (vg == NULL)
    return;
  g_free (vg->id);
  g_free (vg->title);
  g_free (vg->created);
  g_free (vg->audio_file);
  g_free (vg->codec);
  g_free (vg->remote_id);
  if (vg->levels != NULL)
    g_array_unref (vg->levels);
  g_free (vg);
}

gboolean
voicegram_valid_id (const gchar *id)
{
  gsize len;

  if (id == NULL)
    return FALSE;
  len = strlen (id);
  if (len == 0 || len > 64)
    return FALSE;
  for (gsize i = 0; i < len; i++)
    {
      gchar c = id[i];
      if (!(g_ascii_isalnum (c) || c == '-' || c == '_'))
        return FALSE;
    }
  return TRUE;
}

static gboolean
valid_basename (const gchar *name)
{
  return name != NULL && *name != '\0' && *name != '.' &&
         strchr (name, '/') == NULL && strchr (name, '\\') == NULL;
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

gchar *
voicegram_to_xml (const Voicegram *vg)
{
  GString *s = g_string_new ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
  gchar *line;
  gchar num[G_ASCII_DTOSTR_BUF_SIZE];

  line = g_markup_printf_escaped ("<voicegram version=\"%s\" id=\"%s\">\n",
                                  VOICEGRAM_FORMAT_VERSION, vg->id);
  g_string_append (s, line);
  g_free (line);

  line = g_markup_printf_escaped ("  <title>%s</title>\n",
                                  vg->title ? vg->title : "");
  g_string_append (s, line);
  g_free (line);

  line = g_markup_printf_escaped ("  <created>%s</created>\n",
                                  vg->created ? vg->created : "");
  g_string_append (s, line);
  g_free (line);

  line = g_markup_printf_escaped (
      "  <audio file=\"%s\" codec=\"%s\" container=\"ogg\" "
      "duration-ms=\"%" G_GUINT64_FORMAT "\" size=\"%" G_GUINT64_FORMAT "\"/>\n",
      vg->audio_file ? vg->audio_file : "",
      vg->codec ? vg->codec : "audio/x-opus",
      vg->duration_ms, vg->size_bytes);
  g_string_append (s, line);
  g_free (line);

  g_string_append_printf (s, "  <levels interval-ms=\"%u\" count=\"%u\">",
                          vg->interval_ms, vg->levels->len);
  for (guint i = 0; i < vg->levels->len; i++)
    {
      gdouble v = g_array_index (vg->levels, gdouble, i);
      g_ascii_formatd (num, sizeof num, "%.3f", CLAMP (v, 0.0, 1.0));
      if (i > 0)
        g_string_append_c (s, ' ');
      g_string_append (s, num);
    }
  g_string_append (s, "</levels>\n");

  if (vg->remote_id != NULL && voicegram_valid_id (vg->remote_id))
    {
      line = g_markup_printf_escaped ("  <remote id=\"%s\"/>\n", vg->remote_id);
      g_string_append (s, line);
      g_free (line);
    }

  g_string_append (s, "</voicegram>\n");
  return g_string_free (s, FALSE);
}

gchar *
voicegram_xml_path (const gchar *dir, const gchar *id)
{
  gchar *file, *path;

  if (!voicegram_valid_id (id))
    return NULL;
  file = g_strconcat (id, ".xml", NULL);
  path = g_build_filename (dir, file, NULL);
  g_free (file);
  return path;
}

gboolean
voicegram_save (const Voicegram *vg, const gchar *dir, GError **error)
{
  g_autofree gchar *path = voicegram_xml_path (dir, vg->id);
  g_autofree gchar *xml = NULL;

  if (path == NULL)
    {
      g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                   "Invalid voicegram id");
      return FALSE;
    }
  xml = voicegram_to_xml (vg);
  return g_file_set_contents (path, xml, -1, error);   /* atomic replace */
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
  Voicegram *vg;
  GString   *text;
  gboolean   root_seen;
  gboolean   in_title, in_created, in_levels;
} ParseCtx;

static const gchar *
find_attr (const gchar **names, const gchar **values, const gchar *key)
{
  for (gint i = 0; names[i] != NULL; i++)
    if (g_str_equal (names[i], key))
      return values[i];
  return NULL;
}

static void
start_element (GMarkupParseContext *ctx, const gchar *name,
               const gchar **an, const gchar **av,
               gpointer user_data, GError **error)
{
  ParseCtx *p = user_data;
  const gchar *v;

  if (!p->root_seen)
    {
      if (!g_str_equal (name, "voicegram"))
        {
          g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                       "Root element must be <voicegram>");
          return;
        }
      p->root_seen = TRUE;
      v = find_attr (an, av, "id");
      if (!voicegram_valid_id (v))
        {
          g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                       "Missing or invalid voicegram id");
          return;
        }
      p->vg->id = g_strdup (v);
      return;
    }

  g_string_truncate (p->text, 0);

  if (g_str_equal (name, "title"))
    p->in_title = TRUE;
  else if (g_str_equal (name, "created"))
    p->in_created = TRUE;
  else if (g_str_equal (name, "levels"))
    {
      p->in_levels = TRUE;
      if ((v = find_attr (an, av, "interval-ms")) != NULL)
        {
          guint64 ms = g_ascii_strtoull (v, NULL, 10);
          if (ms >= 1 && ms <= 60000)
            p->vg->interval_ms = (guint) ms;
        }
    }
  else if (g_str_equal (name, "audio"))
    {
      if ((v = find_attr (an, av, "file")) != NULL && valid_basename (v))
        {
          g_free (p->vg->audio_file);
          p->vg->audio_file = g_strdup (v);
        }
      if ((v = find_attr (an, av, "codec")) != NULL)
        {
          g_free (p->vg->codec);
          p->vg->codec = g_strndup (v, 64);
        }
      if ((v = find_attr (an, av, "duration-ms")) != NULL)
        p->vg->duration_ms = g_ascii_strtoull (v, NULL, 10);
      if ((v = find_attr (an, av, "size")) != NULL)
        p->vg->size_bytes = g_ascii_strtoull (v, NULL, 10);
    }
  else if (g_str_equal (name, "remote"))
    {
      if ((v = find_attr (an, av, "id")) != NULL && voicegram_valid_id (v))
        {
          g_free (p->vg->remote_id);
          p->vg->remote_id = g_strdup (v);
        }
    }
}

static void
text_cb (GMarkupParseContext *ctx, const gchar *text, gsize len,
         gpointer user_data, GError **error)
{
  ParseCtx *p = user_data;

  if (p->in_title || p->in_created || p->in_levels)
    {
      /* Bound memory use for hostile input. */
      if (p->text->len + len > 4 * 1024 * 1024)
        {
          g_set_error (error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
                       "Element text too large");
          return;
        }
      g_string_append_len (p->text, text, len);
    }
}

static void
parse_levels (ParseCtx *p)
{
  const gchar *s = p->text->str;

  while (*s != '\0' && p->vg->levels->len < VOICEGRAM_MAX_LEVELS)
    {
      gchar *end;
      gdouble d;

      while (g_ascii_isspace (*s))
        s++;
      if (*s == '\0')
        break;
      d = g_ascii_strtod (s, &end);
      if (end == s)
        break;
      if (!isfinite (d))
        d = 0.0;
      d = CLAMP (d, 0.0, 1.0);
      g_array_append_val (p->vg->levels, d);
      s = end;
    }
}

static void
end_element (GMarkupParseContext *ctx, const gchar *name,
             gpointer user_data, GError **error)
{
  ParseCtx *p = user_data;

  if (p->in_title && g_str_equal (name, "title"))
    {
      g_free (p->vg->title);
      p->vg->title = g_strndup (p->text->str, 256);
      p->in_title = FALSE;
    }
  else if (p->in_created && g_str_equal (name, "created"))
    {
      g_free (p->vg->created);
      p->vg->created = g_strndup (p->text->str, 64);
      p->in_created = FALSE;
    }
  else if (p->in_levels && g_str_equal (name, "levels"))
    {
      parse_levels (p);
      p->in_levels = FALSE;
    }
}

Voicegram *
voicegram_from_xml (const gchar *xml, gssize len, GError **error)
{
  static const GMarkupParser parser = {
    start_element, end_element, text_cb, NULL, NULL
  };
  ParseCtx p = { voicegram_new (), g_string_new (NULL), FALSE, FALSE, FALSE, FALSE };
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &p, NULL);
  gboolean ok;

  ok = g_markup_parse_context_parse (ctx, xml, len, error) &&
       g_markup_parse_context_end_parse (ctx, error);

  g_markup_parse_context_free (ctx);
  g_string_free (p.text, TRUE);

  if (!ok)
    {
      voicegram_free (p.vg);
      return NULL;
    }
  if (p.vg->created == NULL)
    p.vg->created = g_strdup ("1970-01-01T00:00:00Z");
  if (p.vg->title == NULL || *p.vg->title == '\0')
    {
      g_free (p.vg->title);
      p.vg->title = g_strdup (p.vg->id);
    }
  return p.vg;
}

Voicegram *
voicegram_load (const gchar *path, GError **error)
{
  g_autofree gchar *contents = NULL;
  gsize len = 0;
  GStatBuf st;

  if (g_stat (path, &st) == 0 && st.st_size > 8 * 1024 * 1024)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                   "Voicegram file is too large");
      return NULL;
    }
  if (!g_file_get_contents (path, &contents, &len, error))
    return NULL;
  return voicegram_from_xml (contents, (gssize) len, error);
}

static gint
compare_newest_first (gconstpointer a, gconstpointer b)
{
  const Voicegram *va = *(Voicegram * const *) a;
  const Voicegram *vb = *(Voicegram * const *) b;

  /* ISO 8601 UTC strings sort lexicographically. */
  return g_strcmp0 (vb->created, va->created);
}

GPtrArray *
voicegram_list (const gchar *dir)
{
  GPtrArray *list = g_ptr_array_new_with_free_func ((GDestroyNotify) voicegram_free);
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *name;

  if (d == NULL)
    return list;

  while ((name = g_dir_read_name (d)) != NULL)
    {
      g_autofree gchar *path = NULL;
      Voicegram *vg;

      if (!g_str_has_suffix (name, ".xml"))
        continue;
      path = g_build_filename (dir, name, NULL);
      vg = voicegram_load (path, NULL);
      if (vg != NULL)
        g_ptr_array_add (list, vg);
    }
  g_dir_close (d);

  g_ptr_array_sort (list, compare_newest_first);
  return list;
}
