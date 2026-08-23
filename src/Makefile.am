AM_CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes

bin_PROGRAMS = acmtool
lib_LTLIBRARIES = libacm.la

include_HEADERS = libacm.h

libacm_la_SOURCES = decode.c util.c
libacm_la_LDFLAGS = -version-info 1:0:0 -no-undefined

noinst_HEADERS = encode.h wavio.h
acmtool_SOURCES = acmtool.c encode.c wavio.c

if USE_LIBAO
acmtool_CFLAGS = $(AO_CFLAGS) $(AM_CFLAGS)
acmtool_LDADD = libacm.la $(AO_LIBS)
else
acmtool_LDADD = libacm.la
endif

