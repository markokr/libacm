dnl Process this file with autoconf to produce a configure script.
AC_PREREQ(2.59)

AC_INIT([libacm], [1.1], [markokr@gmail.com])
AC_CONFIG_SRCDIR([src/libacm.h])
AM_INIT_AUTOMAKE
AC_CONFIG_HEADERS([config.h])

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl Checks for programs.
AC_PROG_AWK
AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S

AC_DISABLE_STATIC
AM_PROG_LIBTOOL
AM_PROG_CC_C_O

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

dnl Check for XMMS2
xmms2_plugin=no
AC_ARG_ENABLE([xmms2],
  [  --enable-xmms2          Compile plugin for XMMS2],
  [xmms2_plugin=$enableval])
if test $xmms2_plugin = yes; then
  PKG_CHECK_MODULES([XMMS2], [glib-2.0 xmms2-plugin], ,
		    [AC_MSG_ERROR([*** XMMS2 not found ***])])

  AC_MSG_CHECKING([for location of XMMS2 input plugins])
  AC_ARG_WITH([xmms2-plugindir],
    [  --with-xmms2-plugindir=DIR    Destination for XMMS2 plugin (autodetect)],
    [XMMS2_DEST="$withval"])
  test -z "$XMMS2_DEST" && \
  XMMS2_DEST=`$PKG_CONFIG --variable=libdir xmms2-plugin`/xmms2
  AC_MSG_RESULT([$XMMS2_DEST])

  AC_SUBST([XMMS2_CFLAGS])
  AC_SUBST([XMMS2_LIBS])
  AC_SUBST([XMMS2_DEST])
fi

dnl Check for audacious
audacious_plugin=no
AC_ARG_ENABLE([audacious],
  [  --enable-audacious      Compile plugin for Audacious],
  [audacious_plugin=$enableval])
if test $audacious_plugin = yes; then
  PKG_CHECK_MODULES([AUDACIOUS], [audacious], ,
  	[AC_MSG_ERROR([*** Audacious not found ***])])

  AC_MSG_CHECKING([if Audacious >= 1.5])
  PKG_CHECK_EXISTS([audacious >= 1.5.0], [aud_150=yes], [aud_150=no])
  AC_MSG_RESULT([$aud_150])
  if test "$aud_150" = "no"; then
    AC_MSG_WARN([Audacious plugin has been tested only with Audacious version 1.5])
    AC_MSG_WARN([On older versions you are on your own.  Feel free to send patches.])
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
AC_ARG_ENABLE([gstreamer],
  [  --enable-gstreamer       Compile plugin for GStreamer 0.10],
  [gst10_plugin=$enableval])
if test $gst10_plugin = yes; then
  PKG_CHECK_MODULES([GST10], [gstreamer-0.10], ,
     [AC_MSG_ERROR([*** GStreamer0.10 not found ***])])

  AC_MSG_CHECKING([for location of GStreamer plugins])
  AC_ARG_WITH([gstreamer-plugindir], 
    [  --with-gstreamer-plugindir=DIR  Destination for Gstreamer plugin (autodetect)],
    [GST10_DEST="$withval"])
  test -z "$GST10_DEST" && \
  GST10_DEST=`$PKG_CONFIG --variable=libdir gstreamer-0.10`/gstreamer-0.10
  AC_MSG_RESULT([$GST10_DEST])

  AC_SUBST([GST10_CFLAGS])
  AC_SUBST([GST10_LIBS])
  AC_SUBST([GST10_DEST])
fi

dnl Check for libao
have_ao=no
PKG_CHECK_MODULES([AO], [ao], [have_ao=yes],
   [AC_MSG_WARN([*** libao-dev not found, audio output disabled ***])])

if test "$have_ao" = "yes"; then
  AC_SUBST([AO_CFLAGS])
  AC_SUBST([AO_LIBS])
  AC_DEFINE([HAVE_AO], 1, [Define 1 if libao is usable])
fi

AM_CONDITIONAL(MAKE_XMMS2_PLUGIN, test "$xmms2_plugin" = "yes")
AM_CONDITIONAL(MAKE_AUDACIOUS_PLUGIN, test "$audacious_plugin" = "yes")
AM_CONDITIONAL(MAKE_GST10_PLUGIN, test "$gst10_plugin" = "yes")
AM_CONDITIONAL(USE_LIBAO, test "$have_ao" = "yes")

AC_CONFIG_FILES([Makefile src/Makefile plugins/Makefile])
AC_OUTPUT


echo ""
echo "Audio output:         $have_ao"
echo ""
echo "Plugins:"
echo "  XMMS2:              $xmms2_plugin"
echo "  Audacious:          $audacious_plugin"
echo "  Gstreamer 0.10:     $gst10_plugin"
echo ""

