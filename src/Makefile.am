bin_PROGRAMS = gnome-voice

gnome_voice_SOURCES = \
	main.c \
	voicegram.c voicegram.h \
	voice-audio.c voice-audio.h \
	voice-net.c voice-net.h

gnome_voice_CPPFLAGS = \
	-DG_LOG_DOMAIN=\"GNOME-Voice\" \
	-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_74

gnome_voice_CFLAGS = \
	$(GLIB_CFLAGS) $(GTK_CFLAGS) $(GSTREAMER_CFLAGS) $(OPENSSL_CFLAGS) \
	-Wall -Wextra -Wno-unused-parameter

gnome_voice_LDADD = \
	$(GLIB_LIBS) $(GTK_LIBS) $(GSTREAMER_LIBS) $(OPENSSL_LIBS) $(LIBM)
