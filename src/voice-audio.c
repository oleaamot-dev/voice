/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "voice-audio.h"

#include <math.h>
#include <gio/gio.h>
#include <gst/gst.h>

#define LEVEL_INTERVAL_NS  G_GUINT64_CONSTANT (100000000)   /* 100 ms */

/* Recording pipeline: microphone -> mono 48 kHz -> level meter -> Opus -> Ogg -> file */
#define RECORD_PIPELINE                                           \
  "autoaudiosrc ! audioconvert ! audioresample ! "                \
  "audio/x-raw,rate=48000,channels=1 ! "                          \
  "level name=meter interval=100000000 ! "                        \
  "opusenc audio-type=voice bitrate=32000 ! oggmux ! "            \
  "filesink name=sink"

/* ================================================================== */
/* Recorder                                                            */
/* ================================================================== */

struct _VoiceRecorder {
  GstElement     *pipeline;
  guint           bus_watch;
  VoiceLevelFunc  level_cb;
  VoiceDoneFunc   done_cb;
  gpointer        user_data;
  guint64         last_ms;
  gboolean        stopping;
};

VoiceRecorder *
voice_recorder_new (VoiceLevelFunc level_cb, VoiceDoneFunc done_cb, gpointer user_data)
{
  VoiceRecorder *r = g_new0 (VoiceRecorder, 1);
  r->level_cb = level_cb;
  r->done_cb = done_cb;
  r->user_data = user_data;
  return r;
}

gboolean
voice_recorder_is_active (VoiceRecorder *r)
{
  return r->pipeline != NULL;
}

/* The "peak" field of a level message holds one dB value per channel. */
static gdouble
peak_from_level (const GstStructure *s)
{
  const GValue *v = gst_structure_get_value (s, "peak");
  gdouble best_db = -200.0;

  if (v == NULL)
    return 0.0;

  if (GST_VALUE_HOLDS_LIST (v))
    {
      for (guint i = 0; i < gst_value_list_get_size (v); i++)
        best_db = MAX (best_db, g_value_get_double (gst_value_list_get_value (v, i)));
    }
  else
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      if (G_VALUE_HOLDS (v, G_TYPE_VALUE_ARRAY))
        {
          GValueArray *arr = g_value_get_boxed (v);
          for (guint i = 0; arr != NULL && i < arr->n_values; i++)
            best_db = MAX (best_db, g_value_get_double (g_value_array_get_nth (arr, i)));
        }
      G_GNUC_END_IGNORE_DEPRECATIONS
    }

  if (!isfinite (best_db) || best_db <= -200.0)
    return 0.0;
  return CLAMP (pow (10.0, best_db / 20.0), 0.0, 1.0);
}

static void
recorder_finish (VoiceRecorder *r, gboolean ok, const gchar *message)
{
  guint64 duration = r->last_ms;

  /* The bus watch source is removed by returning G_SOURCE_REMOVE. */
  r->bus_watch = 0;
  gst_element_set_state (r->pipeline, GST_STATE_NULL);
  gst_object_unref (r->pipeline);
  r->pipeline = NULL;
  r->stopping = FALSE;

  if (r->done_cb != NULL)
    r->done_cb (ok, duration, message, r->user_data);
}

static gboolean
recorder_bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data)
{
  VoiceRecorder *r = user_data;

  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_ELEMENT:
      {
        const GstStructure *s = gst_message_get_structure (msg);
        if (s != NULL && gst_structure_has_name (s, "level"))
          {
            GstClockTime end = GST_CLOCK_TIME_NONE;
            if (gst_structure_get_clock_time (s, "endtime", &end) &&
                GST_CLOCK_TIME_IS_VALID (end))
              r->last_ms = end / GST_MSECOND;
            if (r->level_cb != NULL && !r->stopping)
              r->level_cb (peak_from_level (s), r->last_ms, r->user_data);
          }
        break;
      }
    case GST_MESSAGE_EOS:
      recorder_finish (r, TRUE, NULL);
      return G_SOURCE_REMOVE;

    case GST_MESSAGE_ERROR:
      {
        GError *err = NULL;
        gst_message_parse_error (msg, &err, NULL);
        recorder_finish (r, FALSE, err ? err->message : "Unknown GStreamer error");
        g_clear_error (&err);
        return G_SOURCE_REMOVE;
      }
    default:
      break;
    }
  return G_SOURCE_CONTINUE;
}

gboolean
voice_recorder_start (VoiceRecorder *r, const gchar *path, GError **error)
{
  GstElement *sink;
  GstBus *bus;
  GError *perr = NULL;

  g_return_val_if_fail (r->pipeline == NULL, FALSE);

  r->pipeline = gst_parse_launch (RECORD_PIPELINE, &perr);
  if (r->pipeline == NULL)
    {
      g_propagate_prefixed_error (error, perr,
          "Cannot build recording pipeline (are gst-plugins-base/good installed?): ");
      return FALSE;
    }
  g_clear_error (&perr);   /* recoverable parse warnings */

  sink = gst_bin_get_by_name (GST_BIN (r->pipeline), "sink");
  g_object_set (sink, "location", path, NULL);
  gst_object_unref (sink);

  r->last_ms = 0;
  r->stopping = FALSE;

  bus = gst_element_get_bus (r->pipeline);
  r->bus_watch = gst_bus_add_watch (bus, recorder_bus_cb, r);
  gst_object_unref (bus);

  if (gst_element_set_state (r->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
      g_source_remove (r->bus_watch);
      r->bus_watch = 0;
      gst_element_set_state (r->pipeline, GST_STATE_NULL);
      gst_object_unref (r->pipeline);
      r->pipeline = NULL;
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Cannot start the microphone (check the sound settings and permissions)");
      return FALSE;
    }
  return TRUE;
}

void
voice_recorder_stop (VoiceRecorder *r)
{
  if (r->pipeline == NULL || r->stopping)
    return;
  r->stopping = TRUE;
  /* EOS lets oggmux write the final pages; recorder_bus_cb() finishes up. */
  gst_element_send_event (r->pipeline, gst_event_new_eos ());
}

void
voice_recorder_free (VoiceRecorder *r)
{
  if (r == NULL)
    return;
  if (r->pipeline != NULL)
    {
      if (r->bus_watch != 0)
        g_source_remove (r->bus_watch);
      gst_element_set_state (r->pipeline, GST_STATE_NULL);
      gst_object_unref (r->pipeline);
    }
  g_free (r);
}

/* ================================================================== */
/* Player                                                              */
/* ================================================================== */

struct _VoicePlayer {
  GstElement    *playbin;
  guint          bus_watch;
  VoiceDoneFunc  done_cb;
  gpointer       user_data;
};

VoicePlayer *
voice_player_new (VoiceDoneFunc done_cb, gpointer user_data)
{
  VoicePlayer *p = g_new0 (VoicePlayer, 1);
  p->done_cb = done_cb;
  p->user_data = user_data;
  return p;
}

gboolean
voice_player_is_playing (VoicePlayer *p)
{
  return p->playbin != NULL;
}

static void
player_teardown (VoicePlayer *p)
{
  gst_element_set_state (p->playbin, GST_STATE_NULL);
  gst_object_unref (p->playbin);
  p->playbin = NULL;
}

static gboolean
player_bus_cb (GstBus *bus, GstMessage *msg, gpointer user_data)
{
  VoicePlayer *p = user_data;

  switch (GST_MESSAGE_TYPE (msg))
    {
    case GST_MESSAGE_EOS:
      p->bus_watch = 0;
      player_teardown (p);
      if (p->done_cb != NULL)
        p->done_cb (TRUE, 0, NULL, p->user_data);
      return G_SOURCE_REMOVE;

    case GST_MESSAGE_ERROR:
      {
        GError *err = NULL;
        gst_message_parse_error (msg, &err, NULL);
        p->bus_watch = 0;
        player_teardown (p);
        if (p->done_cb != NULL)
          p->done_cb (FALSE, 0, err ? err->message : "Playback error", p->user_data);
        g_clear_error (&err);
        return G_SOURCE_REMOVE;
      }
    default:
      break;
    }
  return G_SOURCE_CONTINUE;
}

gboolean
voice_player_play (VoicePlayer *p, const gchar *path, GError **error)
{
  gchar *uri;
  GstBus *bus;

  voice_player_stop (p);

  p->playbin = gst_element_factory_make ("playbin", "voice-player");
  if (p->playbin == NULL)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "GStreamer playbin is missing (install gst-plugins-base)");
      return FALSE;
    }

  uri = gst_filename_to_uri (path, error);
  if (uri == NULL)
    {
      gst_object_unref (p->playbin);
      p->playbin = NULL;
      return FALSE;
    }
  g_object_set (p->playbin, "uri", uri, NULL);
  g_free (uri);

  bus = gst_element_get_bus (p->playbin);
  p->bus_watch = gst_bus_add_watch (bus, player_bus_cb, p);
  gst_object_unref (bus);

  if (gst_element_set_state (p->playbin, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
      g_source_remove (p->bus_watch);
      p->bus_watch = 0;
      player_teardown (p);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot start playback");
      return FALSE;
    }
  return TRUE;
}

void
voice_player_stop (VoicePlayer *p)
{
  if (p->playbin == NULL)
    return;
  if (p->bus_watch != 0)
    {
      g_source_remove (p->bus_watch);
      p->bus_watch = 0;
    }
  player_teardown (p);
}

void
voice_player_free (VoicePlayer *p)
{
  if (p == NULL)
    return;
  voice_player_stop (p);
  g_free (p);
}
