AC_PREREQ([2.69])
AC_INIT([gnome-voice], [51.0],
        [https://github.com/oleaamot-dev/voice/issues],
        [gnome-voice],
        [https://www.gnomevoice.org/])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([1.13 foreign subdir-objects dist-xz no-dist-gzip tar-pax])
AM_SILENT_RULES([yes])

AC_PROG_CC
AC_PROG_INSTALL
PKG_PROG_PKG_CONFIG

dnl ---- Dependencies -------------------------------------------------------
PKG_CHECK_MODULES([GLIB],      [glib-2.0 >= 2.74 gio-2.0 >= 2.74])
PKG_CHECK_MODULES([GTK],       [gtk4 >= 4.10])
PKG_CHECK_MODULES([GSTREAMER], [gstreamer-1.0 >= 1.20])
PKG_CHECK_MODULES([OPENSSL],   [openssl >= 1.1.1])
AC_SEARCH_LIBS([pow], [m], [LIBM="$LIBS"; LIBS=""], [AC_MSG_ERROR([libm not found])])
AC_SUBST([LIBM])

dnl ---- Default Voice server -----------------------------------------------
AC_ARG_WITH([server],
  [AS_HELP_STRING([--with-server=HOST], [default voice server @<:@gnomevoice.org@:>@])],
  [], [with_server=gnomevoice.org])
AC_DEFINE_UNQUOTED([GNOME_VOICE_DEFAULT_HOST], ["$with_server"],
  [Default host for uploading and downloading voice messages])
AC_DEFINE([GNOME_VOICE_DEFAULT_PORT], ["443"], [Default HTTPS port])

AC_CONFIG_FILES([Makefile src/Makefile data/Makefile])
AC_OUTPUT

AC_MSG_NOTICE([
    gnome-voice $VERSION
    prefix:         $prefix
    default server: $with_server
])
