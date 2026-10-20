# GNOME Voice 51.0

Record voice messages, keep a **Voicegram** (XML metadata + level history) for
each one, and upload/download messages to **gnomevoice.org** over TLS.

* Audio: GStreamer — microphone → mono 48 kHz → `level` meter → Opus → Ogg
* Network: OpenSSL, TLS 1.2+, certificate **and hostname** verification always on
* Storage: `~/GNOME.voice/<id>.ogg` + `~/GNOME.voice/<id>.xml`
* Build system: GNU autotools

## Build

Debian/Ubuntu:

    sudo apt install build-essential autoconf automake pkg-config \
         libgtk-4-dev libgstreamer1.0-dev libssl-dev \
         gstreamer1.0-plugins-base gstreamer1.0-plugins-good

Fedora:

    sudo dnf install gcc make autoconf automake pkgconf-pkg-config \
         gtk4-devel gstreamer1-devel openssl-devel \
         gstreamer1-plugins-base gstreamer1-plugins-good

Then:

    ./autogen.sh            # runs autoreconf + ./configure
    make
    sudo make install       # or run ./src/gnome-voice from the build tree

`./configure --with-server=HOST` changes the compiled-in default server
(default `gnomevoice.org`).

Requirements: GLib >= 2.74, GTK >= 4.10, GStreamer >= 1.20, OpenSSL >= 1.1.1.

## Use

* **Record / Stop** — saves `<id>.ogg` and `<id>.xml` in `~/GNOME.voice`.
* **Play** — plays the selected message.
* **Upload** — sends the selected message; the server's message id is shown
  in the status bar and stored in the Voicegram (`<remote id="…"/>`).
* **Download** — type a message id, press Download; audio and Voicegram are
  stored locally under that id.

Environment overrides: `GNOME_VOICE_DIR`, `GNOME_VOICE_HOST`, `GNOME_VOICE_PORT`.

## Voicegram XML

    <voicegram version="51.0" id="20261015T140301-8addc971">
      <title>Voice message 2026-10-15 14:03</title>
      <created>2026-10-15T12:03:01.014828Z</created>
      <audio file="20261015T140301-8addc971.ogg" codec="audio/x-opus"
             container="ogg" duration-ms="7400" size="29811"/>
      <levels interval-ms="100" count="74">0.012 0.210 …</levels>
      <remote id="k3x9q2"/>            <!-- only after upload -->
    </voicegram>

`levels` are linear peak values (0..1), one per `interval-ms`; the window draws
them as the live and saved waveform.

## Server protocol

**The gnomevoice.org API is not something I could look up, so this is the
protocol the client implements — adjust `voice-net.c` if the real service
differs.** All requests are HTTPS/1.1 with `Connection: close`.

| Request | Body | Success reply |
|---|---|---|
| `POST /api/v1/voicegrams` | `multipart/form-data`: `voicegram` (`application/xml`), `audio` (`audio/ogg`) | 2xx with `<voicegram id="…"/>` |
| `GET /api/v1/voicegrams/{id}` | – | Voicegram XML |
| `GET /api/v1/voicegrams/{id}/audio` | – | `audio/ogg` (must start with `OggS`) |

Ids match `[A-Za-z0-9_-]{1,64}`; anything else is rejected before it reaches a
URL or a file name. Server-supplied file names are never used: downloads are
always stored as `<id>.ogg` / `<id>.xml`.

## Security notes

* No option to disable certificate verification exists; `SSL_set1_host` ties
  the certificate to the host name and SNI is sent.
* Responses are limited to 64 MiB, uploads to 32 MiB, header lines to 8 KiB;
  sockets have 30 s timeouts and a transfer has a 5 min deadline.
* No authentication is implemented — add a token header in `http_request()`
  if the service requires one.

## License

GPL-3.0-or-later (see the SPDX headers). Add the GPLv3 text as `COPYING`
before distributing.
