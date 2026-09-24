dnl Process this file with autoconf to produce a configure script.
AC_PREREQ([2.69])

AC_INIT([libacm],[1.5],[markokr@gmail.com])
AC_CONFIG_SRCDIR([src/libacm.h])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_PROG_AWK
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AM_PROG_CC_C_O

LT_INIT([disable-shared])  dnl Default for local dev

PKG_PROG_PKG_CONFIG
PKG_INSTALLDIR

old_LIBS="$LIBS"
AC_SEARCH_LIBS([lrintf], [m], [
  STREAM_LIBS="$LIBS"
  LIBS="$old_LIBS"
  AC_SUBST([STREAM_LIBS])
], [
  AC_MSG_ERROR([[unable to find the lrintf() function]])
])

PKG_CHECK_MODULES([SNDFILE], [sndfile >= 1.0.26], [
  have_sndfile=yes
  AC_DEFINE([HAVE_SNDFILE], 1, [Defined if libsndfile is usable])
  if $PKG_CONFIG --atleast-version 1.1 sndfile; then
    AC_DEFINE([HAVE_SNDFILE_1_1], 1, [Defined if libsndfile is version 1.1.0+])
  fi
], [
  have_sndfile=no
  AC_MSG_ERROR([[*** libsndfile-dev not found, cannot do WAV file I/O ***]])
])

PKG_WITH_MODULES([SAMPLERATE], [samplerate], [
  have_samplerate=yes
  AC_DEFINE([HAVE_SAMPLERATE], 1, [Defined if libsamplerate is usable])
], [
  have_samplerate=no
  AC_MSG_WARN([[*** libsamplerate-dev not available, cannot do sample rate conversion ***]])
])

PKG_WITH_MODULES([AO], [ao], [
  have_ao=yes
  AC_DEFINE([HAVE_AO], 1, [Defined if libao is usable])
], [
  have_ao=no
  AC_MSG_WARN([[*** libao-dev not available, audio output disabled ***"]])
])

AC_CONFIG_FILES([Makefile src/Makefile libacm.pc])
AC_OUTPUT

echo ""
echo "libsndfile:    $have_sndfile"
echo "libsamplerate: $have_samplerate"
echo "libao:         $have_ao"
echo ""
