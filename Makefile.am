
AUTOMAKE_OPTIONS = 1.6 foreign

SUBDIRS = src plugins

EXTRA_DIST = autogen.sh

##
## Windows compilation
##

pdir = $(top_srcdir)/plugins
sdir = $(top_srcdir)/src

WCC = i586-mingw32msvc-gcc
WSTRIP = i586-mingw32msvc-strip
WCFLAGS = -O2 -Wall -I$(sdir) -I$(pdir)

WINAMP_SRCS = $(pdir)/plugin-winamp.c $(sdir)/util.c $(sdir)/decode.c
TOOL_SRCS = $(sdir)/acmtool.c $(sdir)/decode.c $(sdir)/util.c \
	    $(sdir)/encode.c $(sdir)/packer.c $(sdir)/subband.c

in_libacm.dll: $(WINAMP_SRCS) $(pdir)/winamp.h $(sdir)/libacm.h
	$(WCC) $(WCFLAGS) -shared -o $@ $(WINAMP_SRCS)
	$(WSTRIP) $@

acmtool.exe: $(TOOL_SRCS) $(sdir)/libacm.h $(sdir)/packer.h $(sdir)/subband.h
	$(WCC) $(WCFLAGS) -o $@ $(TOOL_SRCS)
	$(WSTRIP) $@

clean-local:
	rm -f in_libacm.dll acmtool.exe

# shortcuts
amp: in_libacm.dll
exe: acmtool.exe
zip: acmtool.exe in_libacm.dll
	zip -q9 libacm-$(VERSION)-win32.zip acmtool.exe in_libacm.dll

.PHONY: amp exe zip

