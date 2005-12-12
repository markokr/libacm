
EXTRA_DIST = plugin-xmms.c plugin-gstreamer.c plugin-winamp.c winamp.h

acm_cpp = -I$(top_srcdir)/src
acm_lib = $(top_builddir)/src/libacm.la

if MAKE_XMMS_PLUGIN
xmms_plugindir = $(XMMS_DEST)
xmms_plugin_LTLIBRARIES = xmms-acm.la
endif

if MAKE_BEEP_PLUGIN
beep_plugindir = $(BEEP_DEST)
beep_plugin_LTLIBRARIES = beep-acm.la
endif

if MAKE_AUDACIOUS_PLUGIN
audacious_plugindir = $(AUDACIOUS_DEST)
audacious_plugin_LTLIBRARIES = audacious-acm.la
endif

if MAKE_GST08_PLUGIN
gst08_plugindir = $(GST08_DEST)
gst08_plugin_LTLIBRARIES = acmdec-gst08.la
endif

xmms_acm_la_SOURCES = plugin-xmms.c
xmms_acm_la_CPPFLAGS = -DXMMS $(XMMS_CFLAGS) $(acm_cpp)
xmms_acm_la_LIBADD = $(acm_lib)
xmms_acm_la_LDFLAGS = $(XMMS_LIBS) -module -avoid-version

beep_acm_la_SOURCES = plugin-xmms.c
beep_acm_la_CPPFLAGS = -DBEEP $(BEEP_CFLAGS) $(acm_cpp)
beep_acm_la_LIBADD = $(acm_lib)
beep_acm_la_LDFLAGS = $(BEEP_LIBS) -module -avoid-version

audacious_acm_la_SOURCES = plugin-xmms.c
audacious_acm_la_CPPFLAGS = -DAUDACIOUS $(AUDACIOUS_CFLAGS) $(acm_cpp)
audacious_acm_la_LIBADD = $(acm_lib)
audacious_acm_la_LDFLAGS = $(AUDACIOUS_LIBS) -module -avoid-version

acmdec_gst08_la_SOURCES = plugin-gstreamer.c
acmdec_gst08_la_CPPFLAGS = $(GST08_CFLAGS) $(acm_cpp)
acmdec_gst08_la_LIBADD = $(acm_lib)
acmdec_gst08_la_LDFLAGS = $(GST08_LIBS) -module -avoid-version

