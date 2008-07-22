
bin_PROGRAMS = acmtool
noinst_LTLIBRARIES = libacm.la

noinst_HEADERS = libacm.h

libacm_la_SOURCES = decode.c util.c

acmtool_SOURCES = acmtool.c

if USE_LIBAO
acmtool_CFLAGS = $(AO_CFLAGS)
acmtool_LDADD = libacm.la $(AO_LIBS)
else
acmtool_LDADD = libacm.la
endif

