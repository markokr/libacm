
AUTOMAKE_OPTIONS = 1.6 foreign

SUBDIRS = src plugins

EXTRA_DIST = autogen.sh

#
# html
#

README.html: README
	asciidoc -a toc $<

html: README.html
.PHONY: html

##
## Windows compilation
##

pdir = $(top_srcdir)/plugins
sdir = $(top_srcdir)/src

#
# build libao into acmtool.  libao tree changes:
# - create os_types.h
# - set debug_flag = 0 in ao_wmm.c
#
aodir = win32/libao-0.8.8
ao_cflags = -I$(aodir)/include -DHAVE_WMM=1 -DHAVE_AO=1
ao_srcs = $(aodir)/src/ao_au.c \
	$(aodir)/src/ao_null.c \
	$(aodir)/src/ao_raw.c \
	$(aodir)/src/ao_wav.c \
	$(aodir)/src/ao_wmm.c \
	$(aodir)/src/audio_out.c \
	$(aodir)/src/config.c
ao_libs = -lwinmm

WCC = i586-mingw32msvc-gcc
WSTRIP = i586-mingw32msvc-strip
WCFLAGS = -O2 -Wall -I$(sdir) -I$(pdir)

WINAMP_SRCS = $(pdir)/plugin-winamp.c $(sdir)/util.c $(sdir)/decode.c
TOOL_SRCS = $(sdir)/acmtool.c $(sdir)/decode.c $(sdir)/util.c

in_libacm.dll: $(WINAMP_SRCS) $(pdir)/winamp.h $(sdir)/libacm.h
	$(WCC) $(WCFLAGS) -shared -o $@ $(WINAMP_SRCS)
	$(WSTRIP) $@

acmtool.exe: $(TOOL_SRCS) $(sdir)/libacm.h
	$(WCC) $(WCFLAGS) $(ao_cflags) -o $@ $(TOOL_SRCS) $(ao_srcs) $(ao_libs)
	$(WSTRIP) $@

clean-local:
	rm -f in_libacm.dll acmtool.exe core core.*

# shortcuts
amp: in_libacm.dll
exe: acmtool.exe
zip: acmtool.exe in_libacm.dll
	zip -q9 libacm-$(VERSION)-win32.zip acmtool.exe in_libacm.dll

.PHONY: amp exe zip

