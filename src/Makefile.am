# common flags
MATH_CFLAGS = -ffp-contract=off
WARN_CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wmissing-prototypes
AM_CFLAGS = $(MATH_CFLAGS) $(WARN_CFLAGS)

# targets
bin_PROGRAMS = acmtool
lib_LTLIBRARIES = libacm.la

# libacm
include_HEADERS = libacm.h
libacm_la_SOURCES = decode.c util.c
libacm_la_LDFLAGS = -version-info 1:0:0 -no-undefined

# acmtool
noinst_HEADERS = encode.h stream.h
acmtool_SOURCES = acmtool.c encode.c stream.c
acmtool_CFLAGS = $(AM_CFLAGS) $(AO_CFLAGS) $(SNDFILE_CFLAGS) $(SRC_CFLAGS)
acmtool_LDADD = libacm.la $(AO_LIBS) $(SNDFILE_LIBS) $(SRC_LIBS)
