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

dnl Check for libao
have_ao=no
PKG_CHECK_MODULES([AO], [ao], [have_ao=yes],
   [AC_MSG_WARN([*** libao-dev not found, audio output disabled ***])])
if test "$have_ao" = "yes"; then
  AC_DEFINE([HAVE_AO], 1, [Define 1 if libao is usable])
fi
AM_CONDITIONAL(USE_LIBAO, test "$have_ao" = "yes")

AC_CONFIG_FILES([Makefile src/Makefile libacm.pc])
AC_OUTPUT

echo ""
echo "Audio output:         $have_ao"
echo ""
