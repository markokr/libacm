
EXTRA_DIST = plugin-xmms2.c plugin-audacious.c plugin-gstreamer.c plugin-winamp.c winamp.h

acm_cpp = -I$(top_srcdir)/src
acm_lib = $(top_builddir)/src/libacm.la

modflags = -module -avoid-version -no-undefined

if MAKE_XMMS2_PLUGIN
xmms2_plugindir = $(XMMS2_DEST)
xmms2_plugin_LTLIBRARIES = libxmms_acm.la
endif

if MAKE_AUDACIOUS_PLUGIN
audacious_plugindir = $(AUDACIOUS_DEST)
audacious_plugin_LTLIBRARIES = audacious-acm.la
endif

if MAKE_GST10_PLUGIN
gst10_plugindir = $(GST10_DEST)
gst10_plugin_LTLIBRARIES = acmdec-gst10.la
endif

libxmms_acm_la_SOURCES = plugin-xmms2.c
libxmms_acm_la_CPPFLAGS = $(XMMS2_CFLAGS) $(acm_cpp)
libxmms_acm_la_LIBADD = $(acm_lib)
libxmms_acm_la_LDFLAGS = $(XMMS2_LIBS) $(modflags) -export-symbols-regex XMMS_PLUGIN_DESC

audacious_acm_la_SOURCES = plugin-audacious.c
audacious_acm_la_CPPFLAGS = $(AUDACIOUS_CFLAGS) $(acm_cpp)
audacious_acm_la_LIBADD = $(acm_lib)
audacious_acm_la_LDFLAGS = $(AUDACIOUS_LIBS) $(modflags) -export-symbols-regex get_plugin_info

acmdec_gst10_la_SOURCES = plugin-gstreamer.c
acmdec_gst10_la_CPPFLAGS = $(GST10_CFLAGS) $(acm_cpp)
acmdec_gst10_la_LIBADD = $(acm_lib) -lgstbase-0.10
acmdec_gst10_la_LDFLAGS = $(GST10_LIBS) $(modflags) -export-symbols-regex gst_plugin_desc

