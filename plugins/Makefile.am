
EXTRA_DIST = plugin-xmms.c plugin-audacious.c plugin-gstreamer.c plugin-winamp.c winamp.h

acm_cpp = -I$(top_srcdir)/src
acm_lib = $(top_builddir)/src/libacm.la

if MAKE_XMMS_PLUGIN
xmms_plugindir = $(XMMS_DEST)
xmms_plugin_LTLIBRARIES = xmms-acm.la
endif

if MAKE_AUDACIOUS_PLUGIN
audacious_plugindir = $(AUDACIOUS_DEST)
audacious_plugin_LTLIBRARIES = audacious-acm.la
endif

if MAKE_GST10_PLUGIN
gst10_plugindir = $(GST10_DEST)
gst10_plugin_LTLIBRARIES = acmdec-gst10.la
endif

xmms_acm_la_SOURCES = plugin-xmms.c
xmms_acm_la_CPPFLAGS = $(XMMS_CFLAGS) $(acm_cpp)
xmms_acm_la_LIBADD = $(acm_lib)
xmms_acm_la_LDFLAGS = $(XMMS_LIBS) -module -avoid-version

audacious_acm_la_SOURCES = plugin-audacious.c
audacious_acm_la_CPPFLAGS = $(AUDACIOUS_CFLAGS) $(acm_cpp)
audacious_acm_la_LIBADD = $(acm_lib)
audacious_acm_la_LDFLAGS = $(AUDACIOUS_LIBS) -module -avoid-version

acmdec_gst10_la_SOURCES = plugin-gstreamer.c
acmdec_gst10_la_CPPFLAGS = $(GST10_CFLAGS) $(acm_cpp)
acmdec_gst10_la_LIBADD = $(acm_lib) -lgstbase-0.10
acmdec_gst10_la_LDFLAGS = $(GST10_LIBS) -module -avoid-version

