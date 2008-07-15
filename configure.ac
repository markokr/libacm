dnl Process this file with autoconf to produce a configure script.
AC_PREREQ(2.52)

AC_INIT([libacm], [1.0-svn], [markokr@gmail.com])
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

dnl Additional gcc tuning
if test x"$GCC" = xyes; then
  AC_MSG_CHECKING([for working warning switches])
  good_CFLAGS="$CFLAGS"
  warnings=""
  flags="-Wall -Wextra"
  # turn off noise from Wextra
  flags="$flags -Wno-unused-parameter -Wno-missing-field-initializers"
  # Wextra does not turn those on?
  flags="$flags -Wmissing-prototypes -Wpointer-arith -Wendif-labels"
  flags="$flags -Wdeclaration-after-statement -Wold-style-definition"
  flags="$flags -Wstrict-prototypes -Wundef -Wformat -Wnonnull -Wstrict-overflow"
  for f in $flags; do
    CFLAGS="$good_CFLAGS $warnings $f"
    AC_COMPILE_IFELSE([void foo(void){}], [warnings="$warnings $f"])
  done
  CFLAGS="$good_CFLAGS $warnings"
  AC_MSG_RESULT([$warnings])
fi

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

dnl Check for audacious
audacious_plugin=no
AC_ARG_ENABLE([audacious],
  [  --enable-audacious      Compile plugin for Audacious],
  [audacious_plugin=$enableval])
if test $audacious_plugin = yes; then
  PKG_CHECK_MODULES([AUDACIOUS], [audacious >= 1.2.0], ,
  	[AC_MSG_ERROR([*** Audacious not found ***])])

  AC_MSG_CHECKING([if Audacious >= 1.3])
  PKG_CHECK_EXISTS([audacious >= 1.3.0], [aud_130=yes], [aud_130=no])
  AC_MSG_RESULT([$aud_130])
  if test "$aud_130" = "yes"; then
    AC_DEFINE(HAVE_AUDACIOUS_1_3, 1, [Define to 1 if Audacious >= 1.3])
  fi

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
gst10_plugin=no
AC_ARG_ENABLE([gstreamer10],
  [  --enable-gstreamer10     Compile plugin for GStreamer 0.10],
  [gst10_plugin=$enableval])
if test $gst10_plugin = yes; then
  PKG_CHECK_MODULES([GST10], [gstreamer-0.10], ,
     [AC_MSG_ERROR([*** GStreamer0.10 not found ***])])

  AC_MSG_CHECKING([for location of GStreamer plugins])
  AC_ARG_WITH([gstreamer10-plugindir], 
    [  --with-gstreamer10-plugindir=DIR  Destination for Gstreamer plugin (autodetect)],
    [GST10_DEST="$withval"])
  test -z "$GST10_DEST" && \
  GST10_DEST=`$PKG_CONFIG --variable=libdir gstreamer-0.10`/gstreamer-0.10
  AC_MSG_RESULT([$GST10_DEST])

  AC_SUBST([GST10_CFLAGS])
  AC_SUBST([GST10_LIBS])
  AC_SUBST([GST10_DEST])
fi

AM_CONDITIONAL(MAKE_XMMS_PLUGIN, test "$xmms_plugin" = "yes")
AM_CONDITIONAL(MAKE_AUDACIOUS_PLUGIN, test "$audacious_plugin" = "yes")
AM_CONDITIONAL(MAKE_GST10_PLUGIN, test "$gst10_plugin" = "yes")

AC_CONFIG_FILES([Makefile src/Makefile plugins/Makefile])
AC_OUTPUT


echo ""
echo "Plugins:"
echo "  XMMS:               $xmms_plugin"
echo "  Audacious:          $audacious_plugin"
echo "  Gstreamer 0.10:     $gst10_plugin"
echo ""

