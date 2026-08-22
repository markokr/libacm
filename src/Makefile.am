
bin_PROGRAMS = acmtool
lib_LTLIBRARIES = libacm.la

include_HEADERS = libacm.h

libacm_la_SOURCES = decode.c util.c
libacm_la_LDFLAGS = -version-info 1:0:0

acmtool_SOURCES = acmtool.c

if USE_LIBAO
acmtool_CFLAGS = $(AO_CFLAGS)
acmtool_LDADD = libacm.la $(AO_LIBS)
else
acmtool_LDADD = libacm.la
endif

