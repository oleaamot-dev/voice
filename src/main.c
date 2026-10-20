/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GNOME Voice 51.0
 * Record voice messages with GStreamer, keep a Voicegram XML next to each
 * recording in ~/GNOME.voice, and exchange messages with gnomevoice.org
 * over TLS (OpenSSL).
 */
#include "config.h"

#include <math.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <gst/gst.h>

#include "voicegram.h"
#include "voice-audio.h"
#include "voice-net.h"

typedef struct {
  GApplication  *gapp;

  GtkWidget     *window, *wave, *list, *status;
  GtkWidget     *record_btn, *play_btn, *upload_btn, *download_btn, *id_entry;

  gchar         *data_dir;     /* ~/GNOME.voice */
  gchar         *host, *port;

  GPtrArray     *items;        /* Voicegram*, newest first */
  gchar         *selected_id;

  VoiceRecorder *recorder;
  VoicePlayer   *player;
  Voicegram     *recording;    /* in progress, owned */
  gchar         *recording_path;
  GArray        *live;         /* gdouble levels of the running recording */

  gboolean       refreshing;
  gboolean       finishing;    /* stop requested, waiting for EOS */
  gboolean       busy;         /* network transfer running */
} App;

static App app_state;   /* one instance per process */

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void
set_status (App *app, const gchar *fmt, ...)
{
  va_list ap;
  gchar *msg;

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  gtk_label_set_text (GTK_LABEL (app->status), msg);
  g_free (msg);
}

static Voicegram *
find_selected (App *app)
{
  if (app->items == NULL || app->selected_id == NULL)
    return NULL;
  for (guint i = 0; i < app->items->len; i++)
    {
      Voicegram *vg = g_ptr_array_index (app->items, i);
      if (g_str_equal (vg->id, app->selected_id))
        return vg;
    }
  return NULL;
}

static void
update_buttons (App *app)
{
  gboolean rec = app->recording != NULL;
  gboolean playing = voice_player_is_playing (app->player);
  Voicegram *sel = find_selected (app);

  gtk_button_set_label (GTK_BUTTON (app->record_btn), rec ? "Stop" : "Record");
  if (rec)
    gtk_widget_add_css_class (app->record_btn, "destructive-action");
  else
    gtk_widget_remove_css_class (app->record_btn, "destructive-action");
  gtk_widget_set_sensitive (app->record_btn, !app->finishing && !app->busy);

  gtk_button_set_label (GTK_BUTTON (app->play_btn), playing ? "Stop playback" : "Play");
  gtk_widget_set_sensitive (app->play_btn, !rec && sel != NULL && sel->audio_file != NULL);

  gtk_widget_set_sensitive (app->upload_btn, !rec && !app->busy && sel != NULL);
  gtk_widget_set_sensitive (app->download_btn, !rec && !app->busy);
  gtk_widget_set_sensitive (app->id_entry, !rec && !app->busy);
}

static gchar *
row_markup (const Voicegram *vg)
{
  GDateTime *dt = vg->created ? g_date_time_new_from_iso8601 (vg->created, NULL) : NULL;
  gchar *when = NULL, *size, *markup;
  guint secs = (guint) (vg->duration_ms / 1000);

  if (dt != NULL)
    {
      GDateTime *local = g_date_time_to_local (dt);
      when = g_date_time_format (local, "%Y-%m-%d %H:%M");
      g_date_time_unref (local);
      g_date_time_unref (dt);
    }
  size = g_format_size (vg->size_bytes);
  markup = g_markup_printf_escaped ("<b>%s</b>\n<small>%s · %u:%02u · %s%s%s</small>",
                                    vg->title, when ? when : "", secs / 60, secs % 60, size,
                                    vg->remote_id ? " · remote id " : "",
                                    vg->remote_id ? vg->remote_id : "");
  g_free (when);
  g_free (size);
  return markup;
}

static void
refresh_list (App *app)
{
  GtkListBox *box = GTK_LIST_BOX (app->list);
  GtkListBoxRow *row;

  app->refreshing = TRUE;

  while ((row = gtk_list_box_get_row_at_index (box, 0)) != NULL)
    gtk_list_box_remove (box, GTK_WIDGET (row));

  if (app->items != NULL)
    g_ptr_array_unref (app->items);
  app->items = voicegram_list (app->data_dir);

  for (guint i = 0; i < app->items->len; i++)
    {
      Voicegram *vg = g_ptr_array_index (app->items, i);
      gchar *markup = row_markup (vg);
      GtkWidget *label = gtk_label_new (NULL);

      gtk_label_set_markup (GTK_LABEL (label), markup);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
      gtk_widget_set_margin_top (label, 6);
      gtk_widget_set_margin_bottom (label, 6);
      gtk_widget_set_margin_start (label, 12);
      gtk_widget_set_margin_end (label, 12);
      gtk_list_box_append (box, label);
      g_free (markup);

      if (app->selected_id != NULL && g_str_equal (app->selected_id, vg->id))
        gtk_list_box_select_row (box, gtk_list_box_get_row_at_index (box, (gint) i));
    }

  if (find_selected (app) == NULL)
    g_clear_pointer (&app->selected_id, g_free);

  app->refreshing = FALSE;
  update_buttons (app);
  gtk_widget_queue_draw (app->wave);
}

/* ------------------------------------------------------------------ */
/* Voicegram drawing                                                   */
/* ------------------------------------------------------------------ */

static void
draw_wave (GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data)
{
  App *app = data;
  const GArray *lv = NULL;
  GdkRGBA fg;
  const double step = 5.0, bar_w = 3.0;
  gint slots = MAX (1, (gint) (w / step));

  if (app->recording != NULL)
    lv = app->live;
  else
    {
      Voicegram *sel = find_selected (app);
      if (sel != NULL)
        lv = sel->levels;
    }

  gtk_widget_get_color (GTK_WIDGET (area), &fg);
  gdk_cairo_set_source_rgba (cr, &fg);

  cairo_save (cr);
  cairo_set_source_rgba (cr, fg.red, fg.green, fg.blue, 0.25);
  cairo_rectangle (cr, 0, h / 2.0 - 0.5, w, 1.0);
  cairo_fill (cr);
  cairo_restore (cr);

  if (lv == NULL || lv->len == 0)
    return;

  for (gint s = 0; s < slots; s++)
    {
      gdouble v = 0.0;

      if (app->recording != NULL)
        {
          /* live view: newest sample at the right edge */
          gint idx = (gint) lv->len - slots + s;
          if (idx < 0)
            continue;
          v = g_array_index (lv, gdouble, idx);
        }
      else if (lv->len <= (guint) slots)
        {
          if ((guint) s >= lv->len)
            break;
          v = g_array_index (lv, gdouble, s);
        }
      else
        {
          /* whole message squeezed into the width: max of each bucket */
          guint from = (guint) ((guint64) s * lv->len / (guint) slots);
          guint to = (guint) ((guint64) (s + 1) * lv->len / (guint) slots);
          if (to <= from)
            to = from + 1;
          for (guint i = from; i < to && i < lv->len; i++)
            v = MAX (v, g_array_index (lv, gdouble, i));
        }

      v = sqrt (CLAMP (v, 0.0, 1.0));     /* make quiet speech visible */
      {
        double bh = MAX (2.0, v * (h - 8));
        cairo_rectangle (cr, s * step, (h - bh) / 2.0, bar_w, bh);
      }
    }
  cairo_fill (cr);
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

static void
on_level (gdouble peak, guint64 position_ms, gpointer data)
{
  App *app = data;
  guint secs = (guint) (position_ms / 1000);

  if (app->live->len < VOICEGRAM_MAX_LEVELS)
    g_array_append_val (app->live, peak);
  gtk_widget_queue_draw (app->wave);
  set_status (app, "Recording… %u:%02u", secs / 60, secs % 60);
}

static void
on_record_done (gboolean ok, guint64 duration_ms, const gchar *message, gpointer data)
{
  App *app = data;
  Voicegram *vg = app->recording;
  GError *err = NULL;

  app->recording = NULL;
  app->finishing = FALSE;

  if (vg == NULL)
    return;

  if (ok)
    {
      GStatBuf st;

      vg->duration_ms = duration_ms ? duration_ms
                                    : (guint64) app->live->len * VOICEGRAM_DEFAULT_INTERVAL_MS;
      vg->interval_ms = VOICEGRAM_DEFAULT_INTERVAL_MS;
      g_array_append_vals (vg->levels, app->live->data, app->live->len);
      if (g_stat (app->recording_path, &st) == 0)
        vg->size_bytes = (guint64) st.st_size;

      if (voicegram_save (vg, app->data_dir, &err))
        {
          g_free (app->selected_id);
          app->selected_id = g_strdup (vg->id);
          set_status (app, "Saved %s", vg->title);
        }
      else
        {
          set_status (app, "Could not save the Voicegram: %s", err->message);
          g_clear_error (&err);
        }
    }
  else
    {
      g_unlink (app->recording_path);
      set_status (app, "Recording failed: %s", message ? message : "unknown error");
    }

  g_clear_pointer (&app->recording_path, g_free);
  g_array_set_size (app->live, 0);
  voicegram_free (vg);
  refresh_list (app);
}

static void
on_record_clicked (GtkButton *button, App *app)
{
  Voicegram *vg;
  GError *err = NULL;

  if (app->recording != NULL)
    {
      app->finishing = TRUE;
      voice_recorder_stop (app->recorder);
      set_status (app, "Finishing…");
      update_buttons (app);
      return;
    }

  voice_player_stop (app->player);

  if (g_mkdir_with_parents (app->data_dir, 0700) != 0)
    {
      set_status (app, "Cannot create %s", app->data_dir);
      return;
    }

  vg = voicegram_new_now ();
  vg->audio_file = g_strconcat (vg->id, ".ogg", NULL);
  g_free (app->recording_path);
  app->recording_path = g_build_filename (app->data_dir, vg->audio_file, NULL);

  if (!voice_recorder_start (app->recorder, app->recording_path, &err))
    {
      set_status (app, "%s", err->message);
      g_clear_error (&err);
      g_clear_pointer (&app->recording_path, g_free);
      voicegram_free (vg);
      return;
    }

  app->recording = vg;
  g_array_set_size (app->live, 0);
  set_status (app, "Recording…");
  update_buttons (app);
  gtk_widget_queue_draw (app->wave);
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static void
on_play_done (gboolean ok, guint64 duration_ms, const gchar *message, gpointer data)
{
  App *app = data;

  if (ok)
    set_status (app, "Playback finished");
  else
    set_status (app, "Playback failed: %s", message ? message : "unknown error");
  update_buttons (app);
}

static void
on_play_clicked (GtkButton *button, App *app)
{
  Voicegram *sel = find_selected (app);
  gchar *path;
  GError *err = NULL;

  if (voice_player_is_playing (app->player))
    {
      voice_player_stop (app->player);
      set_status (app, "Stopped");
      update_buttons (app);
      return;
    }
  if (sel == NULL || sel->audio_file == NULL)
    return;

  path = g_build_filename (app->data_dir, sel->audio_file, NULL);
  if (voice_player_play (app->player, path, &err))
    set_status (app, "Playing %s", sel->title);
  else
    {
      set_status (app, "%s", err->message);
      g_clear_error (&err);
    }
  g_free (path);
  update_buttons (app);
}

/* ------------------------------------------------------------------ */
/* Upload / download (worker threads)                                  */
/* ------------------------------------------------------------------ */

typedef struct {
  gchar *host, *port, *dir, *id;
} NetJob;

static void
net_job_free (NetJob *job)
{
  g_free (job->host);
  g_free (job->port);
  g_free (job->dir);
  g_free (job->id);
  g_free (job);
}

static void
upload_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
  NetJob *job = task_data;
  GError *err = NULL;
  Voicegram *vg = NULL;
  gchar *xml_path, *audio_path = NULL, *remote_id = NULL;

  xml_path = voicegram_xml_path (job->dir, job->id);
  vg = xml_path ? voicegram_load (xml_path, &err) : NULL;

  if (vg == NULL)
    {
      if (err == NULL)
        g_set_error (&err, VOICE_NET_ERROR, VOICE_NET_ERROR_INVALID, "Invalid voicegram");
      g_task_return_error (task, err);
    }
  else if (vg->audio_file == NULL)
    g_task_return_new_error (task, VOICE_NET_ERROR, VOICE_NET_ERROR_INVALID,
                             "The Voicegram has no audio file");
  else
    {
      audio_path = g_build_filename (job->dir, vg->audio_file, NULL);
      if (voice_net_upload (job->host, job->port, xml_path, audio_path, &remote_id, &err))
        g_task_return_pointer (task, remote_id, g_free);
      else
        g_task_return_error (task, err);
    }

  g_free (audio_path);
  g_free (xml_path);
  voicegram_free (vg);
}

static void
upload_done (GObject *source, GAsyncResult *res, gpointer data)
{
  App *app = data;
  NetJob *job = g_task_get_task_data (G_TASK (res));
  GError *err = NULL;
  gchar *remote_id = g_task_propagate_pointer (G_TASK (res), &err);

  app->busy = FALSE;

  if (remote_id == NULL)
    {
      set_status (app, "Upload failed: %s", err->message);
      g_clear_error (&err);
    }
  else
    {
      gchar *xml_path = voicegram_xml_path (job->dir, job->id);
      Voicegram *vg = xml_path ? voicegram_load (xml_path, NULL) : NULL;

      if (vg != NULL)
        {
          g_free (vg->remote_id);
          vg->remote_id = g_strdup (remote_id);
          voicegram_save (vg, job->dir, NULL);
          voicegram_free (vg);
        }
      set_status (app, "Uploaded. Share this message id: %s", remote_id);
      g_free (xml_path);
      g_free (remote_id);
    }
  refresh_list (app);
}

static void
download_thread (GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
  NetJob *job = task_data;
  GError *err = NULL;

  if (voice_net_download (job->host, job->port, job->id, job->dir, &err))
    g_task_return_pointer (task, g_strdup (job->id), g_free);
  else
    g_task_return_error (task, err);
}

static void
download_done (GObject *source, GAsyncResult *res, gpointer data)
{
  App *app = data;
  GError *err = NULL;
  gchar *id = g_task_propagate_pointer (G_TASK (res), &err);

  app->busy = FALSE;

  if (id == NULL)
    {
      set_status (app, "Download failed: %s", err->message);
      g_clear_error (&err);
    }
  else
    {
      g_free (app->selected_id);
      app->selected_id = id;     /* takes ownership */
      set_status (app, "Downloaded message %s", id);
      gtk_editable_set_text (GTK_EDITABLE (app->id_entry), "");
    }
  refresh_list (app);
}

static void
start_net_job (App *app, gboolean upload, const gchar *id)
{
  NetJob *job = g_new0 (NetJob, 1);
  GTask *task;

  job->host = g_strdup (app->host);
  job->port = g_strdup (app->port);
  job->dir = g_strdup (app->data_dir);
  job->id = g_strdup (id);

  task = g_task_new (NULL, NULL, upload ? upload_done : download_done, app);
  g_task_set_task_data (task, job, (GDestroyNotify) net_job_free);
  g_task_run_in_thread (task, upload ? upload_thread : download_thread);
  g_object_unref (task);

  app->busy = TRUE;
  set_status (app, "%s %s …", upload ? "Uploading to" : "Downloading from", app->host);
  update_buttons (app);
}

static void
on_upload_clicked (GtkButton *button, App *app)
{
  Voicegram *sel = find_selected (app);

  if (sel != NULL && !app->busy)
    start_net_job (app, TRUE, sel->id);
}

static void
on_download_clicked (GtkWidget *widget, App *app)
{
  gchar *id = g_strstrip (g_strdup (gtk_editable_get_text (GTK_EDITABLE (app->id_entry))));

  if (app->busy)
    ;
  else if (!voicegram_valid_id (id))
    set_status (app, "Enter a message id (letters, digits, '-' and '_')");
  else
    start_net_job (app, FALSE, id);
  g_free (id);
}

/* ------------------------------------------------------------------ */
/* Window                                                              */
/* ------------------------------------------------------------------ */

static void
on_row_selected (GtkListBox *box, GtkListBoxRow *row, App *app)
{
  if (app->refreshing)
    return;

  g_clear_pointer (&app->selected_id, g_free);
  if (row != NULL)
    {
      gint i = gtk_list_box_row_get_index (row);
      if (i >= 0 && (guint) i < app->items->len)
        app->selected_id = g_strdup (((Voicegram *) g_ptr_array_index (app->items, (guint) i))->id);
    }
  voice_player_stop (app->player);
  update_buttons (app);
  gtk_widget_queue_draw (app->wave);
}

static gboolean
on_close_request (GtkWindow *window, App *app)
{
  if (app->recording != NULL || app->busy)
    {
      set_status (app, app->busy ? "Please wait for the transfer to finish"
                                 : "Stop the recording before closing");
      return TRUE;     /* keep the window open */
    }
  return FALSE;
}

static void
activate (GApplication *gapp, gpointer data)
{
  App *app = data;
  GtkWidget *header, *vbox, *scroll, *bottom;

  if (app->window != NULL)
    {
      gtk_window_present (GTK_WINDOW (app->window));
      return;
    }

  g_mkdir_with_parents (app->data_dir, 0700);

  app->window = gtk_application_window_new (GTK_APPLICATION (gapp));
  gtk_window_set_title (GTK_WINDOW (app->window), "GNOME Voice");
  gtk_window_set_default_size (GTK_WINDOW (app->window), 520, 640);
  g_signal_connect (app->window, "close-request", G_CALLBACK (on_close_request), app);

  header = gtk_header_bar_new ();
  gtk_window_set_titlebar (GTK_WINDOW (app->window), header);

  app->record_btn = gtk_button_new_with_label ("Record");
  app->play_btn = gtk_button_new_with_label ("Play");
  app->upload_btn = gtk_button_new_with_label ("Upload");
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), app->record_btn);
  gtk_header_bar_pack_start (GTK_HEADER_BAR (header), app->play_btn);
  gtk_header_bar_pack_end (GTK_HEADER_BAR (header), app->upload_btn);
  g_signal_connect (app->record_btn, "clicked", G_CALLBACK (on_record_clicked), app);
  g_signal_connect (app->play_btn, "clicked", G_CALLBACK (on_play_clicked), app);
  g_signal_connect (app->upload_btn, "clicked", G_CALLBACK (on_upload_clicked), app);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (vbox, 8);
  gtk_widget_set_margin_bottom (vbox, 8);
  gtk_widget_set_margin_start (vbox, 8);
  gtk_widget_set_margin_end (vbox, 8);
  gtk_window_set_child (GTK_WINDOW (app->window), vbox);

  app->wave = gtk_drawing_area_new ();
  gtk_drawing_area_set_content_height (GTK_DRAWING_AREA (app->wave), 120);
  gtk_widget_set_hexpand (app->wave, TRUE);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (app->wave), draw_wave, app, NULL);
  gtk_box_append (GTK_BOX (vbox), app->wave);

  app->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (app->list), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (app->list, "boxed-list");
  g_signal_connect (app->list, "row-selected", G_CALLBACK (on_row_selected), app);

  scroll = gtk_scrolled_window_new ();
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), app->list);
  gtk_box_append (GTK_BOX (vbox), scroll);

  bottom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  app->id_entry = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (app->id_entry), "Message id to download");
  gtk_widget_set_hexpand (app->id_entry, TRUE);
  app->download_btn = gtk_button_new_with_label ("Download");
  g_signal_connect (app->id_entry, "activate", G_CALLBACK (on_download_clicked), app);
  g_signal_connect (app->download_btn, "clicked", G_CALLBACK (on_download_clicked), app);
  gtk_box_append (GTK_BOX (bottom), app->id_entry);
  gtk_box_append (GTK_BOX (bottom), app->download_btn);
  gtk_box_append (GTK_BOX (vbox), bottom);

  app->status = gtk_label_new (NULL);
  gtk_label_set_xalign (GTK_LABEL (app->status), 0.0f);
  gtk_label_set_wrap (GTK_LABEL (app->status), TRUE);
  gtk_label_set_ellipsize (GTK_LABEL (app->status), PANGO_ELLIPSIZE_END);
  gtk_box_append (GTK_BOX (vbox), app->status);

  app->recorder = voice_recorder_new (on_level, on_record_done, app);
  app->player = voice_player_new (on_play_done, app);

  refresh_list (app);
  set_status (app, "Server: %s · Voicegrams: %s", app->host, app->data_dir);
  gtk_window_present (GTK_WINDOW (app->window));
}

static void
shutdown_cb (GApplication *gapp, gpointer data)
{
  App *app = data;

  voice_recorder_free (app->recorder);
  voice_player_free (app->player);
  if (app->items != NULL)
    g_ptr_array_unref (app->items);
  voicegram_free (app->recording);
  if (app->live != NULL)
    g_array_unref (app->live);
  g_free (app->recording_path);
  g_free (app->selected_id);
  g_free (app->data_dir);
  g_free (app->host);
  g_free (app->port);
}

static gint
local_options_cb (GApplication *gapp, GVariantDict *options, gpointer data)
{
  if (g_variant_dict_contains (options, "version"))
    {
      g_print ("gnome-voice %s\n", PACKAGE_VERSION);
      return 0;
    }
  return -1;     /* continue normally */
}

int
main (int argc, char **argv)
{
  App *app = &app_state;
  GtkApplication *gtk_app;
  const gchar *dir_env = g_getenv ("GNOME_VOICE_DIR");
  int status;

  gst_init (&argc, &argv);

  app->host = g_strdup (voice_net_default_host ());
  app->port = g_strdup (voice_net_default_port ());
  app->data_dir = (dir_env != NULL && *dir_env != '\0')
                  ? g_strdup (dir_env)
                  : g_build_filename (g_get_home_dir (), "GNOME.voice", NULL);
  app->live = g_array_new (FALSE, FALSE, sizeof (gdouble));

  gtk_app = gtk_application_new ("org.gnome.Voice", G_APPLICATION_DEFAULT_FLAGS);
  app->gapp = G_APPLICATION (gtk_app);
  g_application_add_main_option (app->gapp, "version", 0, G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_NONE, "Show the version and exit", NULL);
  g_signal_connect (gtk_app, "handle-local-options", G_CALLBACK (local_options_cb), app);
  g_signal_connect (gtk_app, "activate", G_CALLBACK (activate), app);
  g_signal_connect (gtk_app, "shutdown", G_CALLBACK (shutdown_cb), app);

  status = g_application_run (app->gapp, argc, argv);
  g_object_unref (gtk_app);
  return status;
}
