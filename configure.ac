dnl Process this file with autoconf to produce a configure script.
AC_PREREQ(2.52)

AC_INIT([libacm], [0.9.2], [markokr@gmail.com])
AC_CONFIG_SRCDIR([src/libacm.h])
AM_INIT_AUTOMAKE
AC_CONFIG_HEADERS([config.h])

dnl Checks for programs.
AC_PROG_AWK
AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S

AC_DISABLE_STATIC
AM_PROG_LIBTOOL

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_SIZE_T

dnl Checks for library functions.
AC_HEADER_STDC

dnl Plugin configuration
PKG_PROG_PKG_CONFIG

dnl Check for XMMS
xmms_plugin=no
AC_ARG_ENABLE([xmms],
  [  --enable-xmms           Compile plugin for XMMS],
  [xmms_plugin=$enableval])
if test $xmms_plugin = yes; then
  AC_CHECK_TOOL(XMMS_CONFIG, xmms-config, false)
  test $XMMS_CONFIG = false && AC_MSG_ERROR([*** XMMS not found ***])
dnl Seems buggy:
dnl  AM_PATH_XMMS(1.0.0, :, AC_MSG_ERROR([*** XMMS not found ***])
  AC_MSG_CHECKING([for XMMS])
  XMMS_CFLAGS=`$XMMS_CONFIG --cflags`
  XMMS_LIBS=`$XMMS_CONFIG --libs`
  XMMS_VERSION=`$XMMS_CONFIG --version`
  AC_MSG_RESULT([$XMMS_VERSION])

  AC_MSG_CHECKING([for location of XMMS input plugins])
  AC_ARG_WITH([xmms-plugindir], 
    [  --with-xmms-plugindir=DIR         Destination for XMMS plugin (autodetect)],
    [XMMS_DEST="$withval"])
  test -z "$XMMS_DEST" && \
  XMMS_DEST=`$XMMS_CONFIG --input-plugin-dir`
  AC_MSG_RESULT([$XMMS_DEST])

  AC_SUBST([XMMS_CFLAGS])
  AC_SUBST([XMMS_LIBS])
  AC_SUBST([XMMS_DEST])
fi

dnl Check for beep
beep_plugin=no
AC_ARG_ENABLE([beep],
  [  --enable-beep           Compile plugin for BEEP],
  [beep_plugin=$enableval])
if test $beep_plugin = yes; then
  PKG_CHECK_MODULES([BEEP], [bmp], ,
  	[AC_MSG_ERROR([*** Beep Media Player not found ***])])

  AC_MSG_CHECKING([for location of BEEP input plugins])
  AC_ARG_WITH([beep-plugindir], 
    [  --with-beep-plugindir=DIR         Destination for BMP plugin (autodetect)],
    [BEEP_DEST="$withval"])
  test -z "$BEEP_DEST" && \
  BEEP_DEST=`$PKG_CONFIG --variable=input_plugin_dir bmp`
  AC_MSG_RESULT([$BEEP_DEST])

  AC_SUBST([BEEP_CFLAGS])
  AC_SUBST([BEEP_LIBS])
  AC_SUBST([BEEP_DEST])
fi

dnl Check for audacious
audacious_plugin=no
AC_ARG_ENABLE([audacious],
  [  --enable-audacious      Compile plugin for Audacious],
  [audacious_plugin=$enableval])
if test $audacious_plugin = yes; then
  PKG_CHECK_MODULES([AUDACIOUS], [audacious], ,
  	[AC_MSG_ERROR([*** Audacious not found ***])])

  AC_MSG_CHECKING([for location of Audacious input plugins])
  AC_ARG_WITH([audacious-plugindir], 
    [  --with-audacious-plugindir=DIR    Destination for Audacious plugin (autodetect)],
    [AUDACIOUS_DEST="$withval"])
  test -z "$AUDACIOUS_DEST" && \
  AUDACIOUS_DEST=`$PKG_CONFIG --variable=input_plugin_dir audacious`
  AC_MSG_RESULT([$AUDACIOUS_DEST])

  AC_SUBST([AUDACIOUS_CFLAGS])
  AC_SUBST([AUDACIOUS_LIBS])
  AC_SUBST([AUDACIOUS_DEST])
fi

dnl Check for gstreamer
gst08_plugin=no
AC_ARG_ENABLE([gstreamer08],
  [  --enable-gstreamer08     Compile plugin for GStreamer 0.8],
  [gst08_plugin=$enableval])
if test $gst08_plugin = yes; then
  PKG_CHECK_MODULES([GST08], [gstreamer-0.8], ,
     [AC_MSG_ERROR([*** GStreamer0.8 not found ***])])

  AC_MSG_CHECKING([for location of GStreamer plugins])
  AC_ARG_WITH([gstreamer08-plugindir], 
    [  --with-gstreamer08-plugindir=DIR  Destination for Gstreamer plugin (autodetect)],
    [GST08_DEST="$withval"])
  test -z "$GST08_DEST" && \
  GST08_DEST=`$PKG_CONFIG --variable=libdir gstreamer-0.8`/gstreamer-0.8
  AC_MSG_RESULT([$GST08_DEST])

  AC_SUBST([GST08_CFLAGS])
  AC_SUBST([GST08_LIBS])
  AC_SUBST([GST08_DEST])
fi

AM_CONDITIONAL(MAKE_XMMS_PLUGIN, test "$xmms_plugin" = "yes")
AM_CONDITIONAL(MAKE_BEEP_PLUGIN, test "$beep_plugin" = "yes")
AM_CONDITIONAL(MAKE_AUDACIOUS_PLUGIN, test "$audacious_plugin" = "yes")
AM_CONDITIONAL(MAKE_GST08_PLUGIN, test "$gst08_plugin" = "yes")

AC_CONFIG_FILES([Makefile src/Makefile plugins/Makefile])
AC_OUTPUT


echo ""
echo "Plugins:"
echo "  XMMS:               $xmms_plugin"
echo "  Beep Media Player:  $beep_plugin"
echo "  Audacious:          $audacious_plugin"
echo "  Gstreamer 0.8:      $gst08_plugin"
echo ""

