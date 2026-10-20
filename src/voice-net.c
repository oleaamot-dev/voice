/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"

#include "voice-net.h"
#include "voicegram.h"

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#define MAX_BODY_BYTES     (64 * 1024 * 1024)
#define MAX_UPLOAD_BYTES   (32 * 1024 * 1024)
#define MAX_LINE_BYTES     8192
#define SOCKET_TIMEOUT_S   30
#define TRANSFER_DEADLINE  (5 * G_TIME_SPAN_MINUTE)

G_DEFINE_QUARK (voice-net-error-quark, voice_net_error)

const gchar *
voice_net_default_host (void)
{
  const gchar *h = g_getenv ("GNOME_VOICE_HOST");
  return (h != NULL && *h != '\0') ? h : GNOME_VOICE_DEFAULT_HOST;
}

const gchar *
voice_net_default_port (void)
{
  const gchar *p = g_getenv ("GNOME_VOICE_PORT");
  return (p != NULL && *p != '\0') ? p : GNOME_VOICE_DEFAULT_PORT;
}

/* ================================================================== */
/* TLS connection                                                      */
/* ================================================================== */

typedef struct {
  SSL_CTX *ctx;
  BIO     *bio;      /* ssl BIO on top of a connect BIO */
  gint64   deadline; /* monotonic µs */
} Conn;

static gchar *
openssl_error (void)
{
  unsigned long e = ERR_get_error ();
  char buf[256];

  if (e == 0)
    return g_strdup ("no details from OpenSSL");
  ERR_error_string_n (e, buf, sizeof buf);
  ERR_clear_error ();
  return g_strdup (buf);
}

static gboolean
host_is_safe (const gchar *host)
{
  if (host == NULL || *host == '\0' || strlen (host) > 253)
    return FALSE;
  for (const gchar *c = host; *c != '\0'; c++)
    if (!(g_ascii_isalnum (*c) || *c == '.' || *c == '-'))
      return FALSE;
  return TRUE;
}

static gboolean
port_is_safe (const gchar *port)
{
  if (port == NULL || *port == '\0' || strlen (port) > 5)
    return FALSE;
  for (const gchar *c = port; *c != '\0'; c++)
    if (!g_ascii_isdigit (*c))
      return FALSE;
  return TRUE;
}

static void
conn_close (Conn *c)
{
  if (c == NULL)
    return;
  if (c->bio != NULL)
    {
      BIO_ssl_shutdown (c->bio);
      BIO_free_all (c->bio);
    }
  if (c->ctx != NULL)
    SSL_CTX_free (c->ctx);
  g_free (c);
}

static Conn *
conn_open (const gchar *host, const gchar *port, GError **error)
{
  Conn *c;
  BIO *tcp = NULL;
  SSL *ssl = NULL;
  gchar *hostport;
  struct timeval tv = { SOCKET_TIMEOUT_S, 0 };
  int fd = -1;
  long vr;

  if (!host_is_safe (host) || !port_is_safe (port))
    {
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_INVALID,
                   "Invalid server name or port");
      return NULL;
    }

  OPENSSL_init_ssl (OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);

  c = g_new0 (Conn, 1);
  c->deadline = g_get_monotonic_time () + TRANSFER_DEADLINE;

  c->ctx = SSL_CTX_new (TLS_client_method ());
  if (c->ctx == NULL)
    goto tls_fail;
  SSL_CTX_set_min_proto_version (c->ctx, TLS1_2_VERSION);
  SSL_CTX_set_verify (c->ctx, SSL_VERIFY_PEER, NULL);
  SSL_CTX_set_mode (c->ctx, SSL_MODE_AUTO_RETRY);
  if (SSL_CTX_set_default_verify_paths (c->ctx) != 1)
    goto tls_fail;

  /* 1. plain TCP connection */
  hostport = g_strdup_printf ("%s:%s", host, port);
  tcp = BIO_new (BIO_s_connect ());
  BIO_set_conn_hostname (tcp, hostport);
  g_free (hostport);
  if (BIO_do_connect (tcp) <= 0)
    {
      gchar *why = openssl_error ();
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_CONNECT,
                   "Cannot connect to %s:%s (%s)", host, port, why);
      g_free (why);
      BIO_free_all (tcp);
      conn_close (c);
      return NULL;
    }
  if (BIO_get_fd (tcp, &fd) >= 0 && fd >= 0)
    {
      setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

  /* 2. TLS on top, with SNI and hostname verification */
  c->bio = BIO_new_ssl (c->ctx, 1);
  if (c->bio == NULL)
    {
      BIO_free_all (tcp);
      goto tls_fail;
    }
  BIO_push (c->bio, tcp);
  BIO_get_ssl (c->bio, &ssl);
  if (ssl == NULL)
    goto tls_fail;
  SSL_set_tlsext_host_name (ssl, host);
  if (SSL_set1_host (ssl, host) != 1)
    goto tls_fail;

  if (BIO_do_handshake (c->bio) <= 0)
    goto tls_fail;

  vr = SSL_get_verify_result (ssl);
  if (vr != X509_V_OK)
    {
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_TLS,
                   "Certificate for %s rejected: %s", host,
                   X509_verify_cert_error_string (vr));
      conn_close (c);
      return NULL;
    }
  return c;

tls_fail:
  {
    gchar *why = openssl_error ();
    g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_TLS,
                 "TLS handshake with %s failed: %s", host, why);
    g_free (why);
  }
  conn_close (c);
  return NULL;
}

static gboolean
conn_write (Conn *c, const guint8 *data, gsize len, GError **error)
{
  while (len > 0)
    {
      int n = BIO_write (c->bio, data, (int) MIN (len, (gsize) 16384));

      if (n > 0)
        {
          data += n;
          len -= (gsize) n;
          continue;
        }
      if (BIO_should_retry (c->bio) && g_get_monotonic_time () < c->deadline)
        continue;

      {
        gchar *why = openssl_error ();
        g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_IO,
                     "Sending data failed: %s", why);
        g_free (why);
      }
      return FALSE;
    }
  return TRUE;
}

/* ---- buffered reader ---------------------------------------------- */

typedef struct {
  Conn      *c;
  GByteArray *buf;
  gsize      pos;
  gboolean   eof;
} Reader;

static gboolean
reader_fill (Reader *r, GError **error)
{
  guint8 tmp[16384];

  if (r->eof)
    return TRUE;

  for (;;)
    {
      int n = BIO_read (r->c->bio, tmp, sizeof tmp);

      if (n > 0)
        {
          /* drop consumed bytes now and then to bound memory */
          if (r->pos > 65536 && r->pos == r->buf->len)
            {
              g_byte_array_set_size (r->buf, 0);
              r->pos = 0;
            }
          g_byte_array_append (r->buf, tmp, (guint) n);
          return TRUE;
        }
      if (BIO_should_retry (r->c->bio))
        {
          if (g_get_monotonic_time () < r->c->deadline)
            continue;
          g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_IO, "Timed out waiting for the server");
          return FALSE;
        }
      /* Orderly close (or close without close_notify): treat as EOF.
       * Truncation is detected by the length checks of the callers. */
      ERR_clear_error ();
      r->eof = TRUE;
      return TRUE;
    }
}

static gboolean
reader_line (Reader *r, GString *out, GError **error)
{
  g_string_truncate (out, 0);
  for (;;)
    {
      guint8 *start = r->buf->data + r->pos;
      gsize avail = r->buf->len - r->pos;
      guint8 *nl = avail ? memchr (start, '\n', avail) : NULL;

      if (nl != NULL)
        {
          gsize n = (gsize) (nl - start);
          g_string_append_len (out, (const gchar *) start, (gssize) n);
          r->pos += n + 1;
          if (out->len > 0 && out->str[out->len - 1] == '\r')
            g_string_truncate (out, out->len - 1);
          return TRUE;
        }
      if (avail > MAX_LINE_BYTES)
        {
          g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL, "HTTP line too long");
          return FALSE;
        }
      if (r->eof)
        {
          g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL, "Connection closed in HTTP headers");
          return FALSE;
        }
      if (!reader_fill (r, error))
        return FALSE;
    }
}

static gboolean
reader_bytes (Reader *r, gsize n, GByteArray *out, GError **error)
{
  while (n > 0)
    {
      gsize avail = r->buf->len - r->pos;

      if (avail == 0)
        {
          if (r->eof)
            {
              g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL,
                           "Response body truncated");
              return FALSE;
            }
          if (!reader_fill (r, error))
            return FALSE;
          continue;
        }
      gsize take = MIN (avail, n);
      if (out != NULL)
        g_byte_array_append (out, r->buf->data + r->pos, (guint) take);
      r->pos += take;
      n -= take;
    }
  return TRUE;
}

/* ================================================================== */
/* HTTP/1.1                                                            */
/* ================================================================== */

typedef struct {
  gint        status;
  GByteArray *body;
} HttpResponse;

static void
http_response_clear (HttpResponse *r)
{
  if (r->body != NULL)
    g_byte_array_unref (r->body);
  r->body = NULL;
}

static gboolean
read_chunked (Reader *r, GByteArray *body, GError **error)
{
  GString *line = g_string_new (NULL);
  gboolean ok = FALSE;

  for (;;)
    {
      gchar *end = NULL;
      guint64 size;
      gchar *semi;

      if (!reader_line (r, line, error))
        goto out;
      if ((semi = strchr (line->str, ';')) != NULL)
        *semi = '\0';
      size = g_ascii_strtoull (g_strstrip (line->str), &end, 16);
      if (end == line->str)
        {
          g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL, "Bad chunk size");
          goto out;
        }
      if (size == 0)
        {
          /* trailers until empty line */
          do
            {
              if (!reader_line (r, line, error))
                goto out;
            }
          while (line->len > 0);
          ok = TRUE;
          goto out;
        }
      if (size > MAX_BODY_BYTES || body->len + size > MAX_BODY_BYTES)
        {
          g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL, "Response too large");
          goto out;
        }
      if (!reader_bytes (r, (gsize) size, body, error))
        goto out;
      if (!reader_line (r, line, error))   /* CRLF after the chunk */
        goto out;
    }
out:
  g_string_free (line, TRUE);
  return ok;
}

static gboolean
http_request (const gchar *host, const gchar *port,
              const gchar *method, const gchar *path,
              const gchar *accept,
              const gchar *content_type, const guint8 *body, gsize body_len,
              HttpResponse *resp, GError **error)
{
  Conn *c = conn_open (host, port, error);
  GString *head, *line = NULL;
  Reader rd = { NULL, NULL, 0, FALSE };
  gboolean ok = FALSE, chunked = FALSE;
  gint64 content_length = -1;
  gchar *host_hdr;

  if (c == NULL)
    return FALSE;

  memset (resp, 0, sizeof *resp);

  host_hdr = g_str_equal (port, "443") ? g_strdup (host) : g_strdup_printf ("%s:%s", host, port);
  head = g_string_new (NULL);
  g_string_append_printf (head,
      "%s %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "User-Agent: GNOME-Voice/%s\r\n"
      "Accept: %s\r\n"
      "Connection: close\r\n",
      method, path, host_hdr, PACKAGE_VERSION, accept);
  if (content_type != NULL)
    g_string_append_printf (head, "Content-Type: %s\r\nContent-Length: %" G_GSIZE_FORMAT "\r\n",
                            content_type, body_len);
  g_string_append (head, "\r\n");
  g_free (host_hdr);

  if (!conn_write (c, (const guint8 *) head->str, head->len, error) ||
      (body_len > 0 && !conn_write (c, body, body_len, error)))
    goto out;

  rd.c = c;
  rd.buf = g_byte_array_new ();
  line = g_string_new (NULL);

  /* status line */
  if (!reader_line (&rd, line, error))
    goto out;
  if (!g_str_has_prefix (line->str, "HTTP/1.") || strlen (line->str) < 12)
    {
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL, "Not an HTTP response");
      goto out;
    }
  resp->status = (gint) g_ascii_strtoll (line->str + 9, NULL, 10);

  /* headers */
  for (;;)
    {
      if (!reader_line (&rd, line, error))
        goto out;
      if (line->len == 0)
        break;
      if (g_ascii_strncasecmp (line->str, "content-length:", 15) == 0)
        {
          gchar *end = NULL;
          guint64 v = g_ascii_strtoull (g_strstrip (line->str + 15), &end, 10);
          if (end == line->str + 15 || v > MAX_BODY_BYTES)
            {
              g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL, "Bad or oversized Content-Length");
              goto out;
            }
          content_length = (gint64) v;
        }
      else if (g_ascii_strncasecmp (line->str, "transfer-encoding:", 18) == 0 &&
               strstr (line->str + 18, "chunked") != NULL)
        chunked = TRUE;
    }

  resp->body = g_byte_array_new ();
  if (resp->status == 204 || resp->status == 304 || (resp->status >= 100 && resp->status < 200))
    ok = TRUE;
  else if (chunked)
    ok = read_chunked (&rd, resp->body, error);
  else if (content_length >= 0)
    ok = reader_bytes (&rd, (gsize) content_length, resp->body, error);
  else
    ok = TRUE;   /* no framing: treat as empty body */

out:
  if (line != NULL)
    g_string_free (line, TRUE);
  if (rd.buf != NULL)
    g_byte_array_unref (rd.buf);
  g_string_free (head, TRUE);
  conn_close (c);
  if (!ok)
    http_response_clear (resp);
  return ok;
}

/* Run a request and require a 2xx status. */
static gboolean
http_request_ok (const gchar *host, const gchar *port, const gchar *method,
                 const gchar *path, const gchar *accept,
                 const gchar *content_type, const guint8 *body, gsize body_len,
                 HttpResponse *resp, GError **error)
{
  if (!http_request (host, port, method, path, accept, content_type, body, body_len, resp, error))
    return FALSE;

  if (resp->status < 200 || resp->status > 299)
    {
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_HTTP,
                   "Server answered HTTP %d", resp->status);
      http_response_clear (resp);
      return FALSE;
    }
  return TRUE;
}

/* ================================================================== */
/* Server API (see README.md)                                          */
/* ================================================================== */

static void
id_start_element (GMarkupParseContext *ctx, const gchar *name,
                  const gchar **an, const gchar **av,
                  gpointer user_data, GError **error)
{
  gchar **out = user_data;

  if (*out != NULL || !g_str_equal (name, "voicegram"))
    return;
  for (gint i = 0; an[i] != NULL; i++)
    if (g_str_equal (an[i], "id") && voicegram_valid_id (av[i]))
      *out = g_strdup (av[i]);
}

static gchar *
parse_remote_id (const GByteArray *xml)
{
  static const GMarkupParser parser = { id_start_element, NULL, NULL, NULL, NULL };
  gchar *id = NULL;
  GMarkupParseContext *ctx = g_markup_parse_context_new (&parser, 0, &id, NULL);

  g_markup_parse_context_parse (ctx, (const gchar *) xml->data, (gssize) xml->len, NULL);
  g_markup_parse_context_free (ctx);
  return id;
}

gboolean
voice_net_upload (const gchar *host, const gchar *port,
                  const gchar *xml_path, const gchar *audio_path,
                  gchar **remote_id, GError **error)
{
  g_autoptr(Voicegram) vg = NULL;
  gchar *xml = NULL, *audio = NULL;
  gsize xml_len = 0, audio_len = 0;
  GStatBuf st;
  gchar *boundary, *ctype, *part;
  GByteArray *body;
  HttpResponse resp;
  gchar *id = NULL;
  gboolean ok = FALSE;

  *remote_id = NULL;

  vg = voicegram_load (xml_path, error);
  if (vg == NULL)
    return FALSE;

  if (g_stat (audio_path, &st) != 0 || st.st_size <= 0 || st.st_size > MAX_UPLOAD_BYTES)
    {
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_INVALID,
                   "Audio file is missing, empty or larger than %d MiB",
                   MAX_UPLOAD_BYTES / (1024 * 1024));
      return FALSE;
    }
  if (!g_file_get_contents (xml_path, &xml, &xml_len, error))
    return FALSE;
  if (!g_file_get_contents (audio_path, &audio, &audio_len, error))
    {
      g_free (xml);
      return FALSE;
    }

  boundary = g_strdup_printf ("----GNOMEVoice%08x%08x", g_random_int (), g_random_int ());
  ctype = g_strdup_printf ("multipart/form-data; boundary=%s", boundary);
  body = g_byte_array_new ();

  part = g_strdup_printf ("--%s\r\nContent-Disposition: form-data; name=\"voicegram\"; "
                          "filename=\"%s.xml\"\r\nContent-Type: application/xml\r\n\r\n",
                          boundary, vg->id);
  g_byte_array_append (body, (guint8 *) part, (guint) strlen (part));
  g_free (part);
  g_byte_array_append (body, (guint8 *) xml, (guint) xml_len);

  part = g_strdup_printf ("\r\n--%s\r\nContent-Disposition: form-data; name=\"audio\"; "
                          "filename=\"%s.ogg\"\r\nContent-Type: audio/ogg\r\n\r\n",
                          boundary, vg->id);
  g_byte_array_append (body, (guint8 *) part, (guint) strlen (part));
  g_free (part);
  g_byte_array_append (body, (guint8 *) audio, (guint) audio_len);

  part = g_strdup_printf ("\r\n--%s--\r\n", boundary);
  g_byte_array_append (body, (guint8 *) part, (guint) strlen (part));
  g_free (part);

  if (http_request_ok (host, port, "POST", "/api/v1/voicegrams", "application/xml",
                       ctype, body->data, body->len, &resp, error))
    {
      id = parse_remote_id (resp.body);
      if (id == NULL)
        g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL,
                     "Server reply did not contain a valid voicegram id");
      else
        {
          *remote_id = id;
          ok = TRUE;
        }
      http_response_clear (&resp);
    }

  g_byte_array_unref (body);
  g_free (ctype);
  g_free (boundary);
  g_free (xml);
  g_free (audio);
  return ok;
}

gboolean
voice_net_download (const gchar *host, const gchar *port,
                    const gchar *remote_id, const gchar *dest_dir,
                    GError **error)
{
  g_autoptr(Voicegram) vg = NULL;
  g_autofree gchar *meta_path = NULL, *audio_path = NULL, *audio_name = NULL;
  gchar *url;
  HttpResponse meta, audio;

  if (!voicegram_valid_id (remote_id))
    {
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_INVALID,
                   "Invalid message id (use letters, digits, '-' and '_')");
      return FALSE;
    }

  /* 1. metadata */
  url = g_strdup_printf ("/api/v1/voicegrams/%s", remote_id);
  if (!http_request_ok (host, port, "GET", url, "application/xml", NULL, NULL, 0, &meta, error))
    {
      g_free (url);
      return FALSE;
    }
  g_free (url);

  vg = voicegram_from_xml ((const gchar *) meta.body->data, (gssize) meta.body->len, error);
  http_response_clear (&meta);
  if (vg == NULL)
    return FALSE;

  /* 2. audio */
  url = g_strdup_printf ("/api/v1/voicegrams/%s/audio", remote_id);
  if (!http_request_ok (host, port, "GET", url, "audio/ogg", NULL, NULL, 0, &audio, error))
    {
      g_free (url);
      return FALSE;
    }
  g_free (url);

  if (audio.body->len < 4 || memcmp (audio.body->data, "OggS", 4) != 0)
    {
      g_set_error (error, VOICE_NET_ERROR, VOICE_NET_ERROR_PROTOCOL,
                   "Server did not return Ogg audio");
      http_response_clear (&audio);
      return FALSE;
    }

  /* 3. store both under the remote id; never trust server-provided file names */
  audio_name = g_strconcat (remote_id, ".ogg", NULL);
  audio_path = g_build_filename (dest_dir, audio_name, NULL);

  if (!g_file_set_contents (audio_path, (const gchar *) audio.body->data,
                            (gssize) audio.body->len, error))
    {
      http_response_clear (&audio);
      return FALSE;
    }

  g_free (vg->id);
  vg->id = g_strdup (remote_id);
  g_free (vg->remote_id);
  vg->remote_id = g_strdup (remote_id);
  g_free (vg->audio_file);
  vg->audio_file = g_strdup (audio_name);
  vg->size_bytes = audio.body->len;
  http_response_clear (&audio);

  return voicegram_save (vg, dest_dir, error);
}
