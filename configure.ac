dnl Process this file with autoconf to produce a configure script.
AC_PREREQ([2.71])

AC_INIT([libacm],[1.3],[markokr@gmail.com])
AC_CONFIG_SRCDIR([src/libacm.h])
AM_INIT_AUTOMAKE
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl Checks for programs.
AC_PROG_AWK
AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
LT_INIT

AC_DISABLE_STATIC
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
    AC_COMPILE_IFELSE([AC_LANG_SOURCE([[void foo(void){}]])], [warnings="$warnings $f"])
  done
  CFLAGS="$good_CFLAGS $warnings"
  AC_MSG_RESULT([$warnings])
fi

dnl Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_SIZE_T

dnl Checks for library functions.
AC_CHECK_INCLUDES_DEFAULT

dnl Plugin configuration
PKG_PROG_PKG_CONFIG

dnl Check for libao
have_ao=no
PKG_CHECK_MODULES([AO], [ao], [have_ao=yes],
   [AC_MSG_WARN([*** libao-dev not found, audio output disabled ***])])

if test "$have_ao" = "yes"; then
  AC_SUBST([AO_CFLAGS])
  AC_SUBST([AO_LIBS])
  AC_DEFINE([HAVE_AO], 1, [Define 1 if libao is usable])
fi

AM_CONDITIONAL(USE_LIBAO, test "$have_ao" = "yes")

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT


echo ""
echo "Audio output:         $have_ao"
echo ""
