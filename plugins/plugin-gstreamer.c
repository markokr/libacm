/*
 * plugin-gstreamer.c - Decoder plugin for GStreamer.
 *
 * Copyright (C) 2005-2008  Marko Kreen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/param.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/base/gstbasesrc.h>

#include "libacm.h"

/* make sure we know byte order */
#if !defined(BYTE_ORDER) || !defined(LITTLE_ENDIAN)
#error BYTE_ORDER must be defined.
#endif
#if BYTE_ORDER == LITTLE_ENDIAN
#define ACM_NATIVE_BE 0
#else
#define ACM_NATIVE_BE 1
#endif

/* default request length */
#define REQLEN (4 * 1024)

/* Property ID for AcmDec->location */
#define PROP_LOCATION 1

/*
 * AcmDec struct
 */
typedef struct AcmDec {
	GstBaseSrc parent;

	gchar *location;
	ACMStream *ctx;
} AcmDec;

typedef struct AcmDecClass {
	GstBaseSrcClass parent_class;
} AcmDecClass;

/*
 * define element class
 */

#define TYPE_ACMDEC        (acmdec_get_type())
#define ACMDEC(o)          G_TYPE_CHECK_INSTANCE_CAST((o),TYPE_ACMDEC,AcmDec)
#define ACMDEC_CLASS(k)    G_TYPE_CHECK_CLASS_CAST((k),TYPE_ACMDEC,AcmDecClass)
#define IS_ACMDEC(o)       G_TYPE_CHECK_INSTANCE_TYPE((o),TYPE_ACMDEC)
#define IS_ACMDEC_CLASS(c) G_TYPE_CHECK_CLASS_TYPE((c),TYPE_ACMDEC)

GST_BOILERPLATE (AcmDec, acmdec, GstBaseSrc, GST_TYPE_BASE_SRC);

static GstElementDetails acmdec_details = {
	"acmdec",
	"Codec/Decoder/Audio",
	"InterPlay ACM Audio decoder",
	"Marko Kreen <markokr@gmail.com>"
};

/*
 * define plugin
 */

static void acmdec_detect_file(GstTypeFind *find, gpointer junk);

static gboolean acmdec_plugin_init(GstPlugin *plugin)
{
	static char *ext_list[] = {"acm", NULL};
	if (0) {
		GstCaps *caps = gst_caps_new_simple("audio/x-acm", NULL);
		if (!gst_type_find_register(plugin, "audio/x-acm", GST_RANK_PRIMARY,
				      	    acmdec_detect_file, ext_list, caps, NULL, NULL))
			return FALSE;
	}

	return gst_element_register(plugin, "acmdec",
				    GST_RANK_PRIMARY, TYPE_ACMDEC);
}

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR, GST_VERSION_MINOR,
	"acmdec",
	"InterPlay ACM Audio Format",
	acmdec_plugin_init,
	LIBACM_VERSION,
	"LGPL",
	"libacm",
	"http://libacm.berlios.de/"
);

/*
 * pad format details
 */

#define BASE_CAPS \
	"audio/x-raw-int, " \
	"endianness = (int) BYTE_ORDER, " \
	"width = (int) 16, " \
	"depth = (int) 16, " \
	"signed = (bool) TRUE"

static GstStaticPadTemplate src_factory =
GST_STATIC_PAD_TEMPLATE(
	"src", GST_PAD_SRC, GST_PAD_ALWAYS,
	GST_STATIC_CAPS(BASE_CAPS ", "
			"rate = (int) [ 4000, 96000 ], "
			"channels = (int) [ 1, 2 ]")
);

/*
 * get/set properties
 */

static void acmdec_get_property(GObject *obj, guint prop_id,
				GValue *value, GParamSpec *pspec)
{
	AcmDec *acm = ACMDEC(obj);

	GST_OBJECT_LOCK(acm);
	switch (prop_id) {
	case PROP_LOCATION:
		if (acm->location)
			g_value_set_string(value, acm->location);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
	GST_OBJECT_UNLOCK(acm);
}

static void acmdec_set_property(GObject *obj, guint prop_id,
				const GValue *value, GParamSpec *pspec)
{
	AcmDec *acm = ACMDEC(obj);

	GST_OBJECT_LOCK(acm);
	switch (prop_id) {
	case PROP_LOCATION:
		if (acm->location)
			g_free(acm->location);
		acm->location = g_value_dup_string(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(obj, prop_id, pspec);
	}
	GST_OBJECT_UNLOCK(acm);
}


static gboolean acmdec_check_get_range(GstBaseSrc *base)
{
	return TRUE;
}

static gboolean acmdec_is_seekable(GstBaseSrc *base)
{
	return TRUE;
}

/* return data size on bytes */
static int acmdec_get_size(GstBaseSrc *base, guint64 *size_p)
{
	AcmDec *acm = ACMDEC(base);
	const ACMInfo *info;
	if (!acm->ctx)
		return FALSE;

	info = acm_info(acm->ctx);
	*size_p = acm_pcm_total(acm->ctx) * info->channels * 2;
	return TRUE;
}

/*
 * caps negotation
 */

static GstCaps *acmdec_get_caps(GstBaseSrc *base)
{
	AcmDec *acm = ACMDEC(base);
	const ACMInfo *info;
	GstCaps *caps;
	GstStructure *s;
	unsigned i;

	caps = gst_caps_copy (gst_pad_get_pad_template_caps (base->srcpad));
	if (!caps || !acm->ctx)
		return caps;

	info = acm_info(acm->ctx);
	for (i = 0; i < gst_caps_get_size (caps); i++) {
		s = gst_caps_get_structure (caps, i);
		gst_structure_set (s, "channels", G_TYPE_INT, info->channels,
				   "rate", G_TYPE_INT, info->rate, NULL);
	}

	return caps;
}


/*
 * start/stop/produce sound
 */

static gboolean acmdec_start(GstBaseSrc *base)
{
	int res;
	AcmDec *acm = ACMDEC(base);

	if (!acm->location || !acm->location[0]) {
		GST_ELEMENT_ERROR(acm, RESOURCE, OPEN_READ,
				  ("No location specified"), (NULL));
		return FALSE;
	}

	res = acm_open_file(&acm->ctx, acm->location);
	if (res < 0) {
		GST_ELEMENT_ERROR(acm, RESOURCE, OPEN_READ,
				  ("Cannot open file"), (NULL));
		return FALSE;
	}

	if (0) {
		const ACMInfo *info;
		GstCaps *caps;
		info = acm_info(acm->ctx);
		caps = gst_caps_from_string(BASE_CAPS);
		gst_caps_set_simple(caps,
				    "channels", G_TYPE_INT, info->channels,
				    "rate", G_TYPE_INT, info->rate, NULL);
		res = gst_pad_set_caps(base->srcpad, caps);
		if (!res) {
			GST_ELEMENT_ERROR(acm, CORE, NEGOTIATION, (NULL), (NULL));
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean acmdec_stop(GstBaseSrc *base)
{
	AcmDec *acm = ACMDEC(base);

	if (acm->ctx) {
		acm_close(acm->ctx);
		acm->ctx = NULL;
	}
	return TRUE;
}

static GstFlowReturn acmdec_create(GstBaseSrc *base, guint64 offset, guint size, GstBuffer **buf)
{
	AcmDec *acm = ACMDEC(base);
	const ACMInfo *info = acm_info(acm->ctx);
	void *data;
	int req_pos, new_pos;
	int got;

	req_pos = offset / (2 * info->channels);
	if (acm_pcm_tell(acm->ctx) != req_pos) {
		printf("seeking: cur=%d, new=%d", acm_pcm_tell(acm->ctx), req_pos);
		new_pos = acm_seek_pcm(acm->ctx, req_pos);
		if (new_pos != req_pos)
			return GST_FLOW_UNEXPECTED;
	}

	*buf = gst_buffer_new_and_alloc (size);
	data = GST_BUFFER_DATA (*buf);
	GST_BUFFER_SIZE (*buf) = 0;

	got = acm_read_loop(acm->ctx, data, size, ACM_NATIVE_BE, 2, 1);
	if (got <= 0) {
		gst_buffer_unref (*buf);
		*buf = NULL;
		return GST_FLOW_UNEXPECTED;
	}
	GST_BUFFER_SIZE (*buf) = got;

	gst_buffer_set_caps(*buf, GST_PAD_CAPS (base->srcpad));

	return GST_FLOW_OK;
}

/*
 * Object initialization
 */

static void acmdec_init(AcmDec *acm, AcmDecClass *klass)
{
	acm->location = NULL;
	acm->ctx = NULL;
}

static void acmdec_finalize(GObject *obj)
{
	AcmDec *acm = ACMDEC(obj);
	GObjectClass *gobj_parent = G_OBJECT_CLASS(parent_class);

	if (acm->location) {
		g_free(acm->location);
		acm->location = NULL;
	}

	if (acm->ctx) {
		acm_close(acm->ctx);
		acm->ctx = NULL;
	}

	if (gobj_parent->finalize)
		gobj_parent->finalize(obj);
}

/*
 * Class initialization
 */

static void acmdec_class_init(AcmDecClass *acm_class)
{
	GObjectClass *gobj_class = G_OBJECT_CLASS(acm_class);
	GstBaseSrcClass *bsrc_class = GST_BASE_SRC_CLASS(acm_class);
	
	gobj_class->finalize = GST_DEBUG_FUNCPTR (acmdec_finalize);
	gobj_class->set_property = GST_DEBUG_FUNCPTR (acmdec_set_property);
	gobj_class->get_property = GST_DEBUG_FUNCPTR (acmdec_get_property);
	
	bsrc_class->start = GST_DEBUG_FUNCPTR (acmdec_start);
	bsrc_class->stop = GST_DEBUG_FUNCPTR (acmdec_stop);
	bsrc_class->create = GST_DEBUG_FUNCPTR (acmdec_create);

	bsrc_class->get_caps = GST_DEBUG_FUNCPTR (acmdec_get_caps);
	bsrc_class->get_size = GST_DEBUG_FUNCPTR (acmdec_get_size);
	bsrc_class->is_seekable = GST_DEBUG_FUNCPTR (acmdec_is_seekable);
	bsrc_class->check_get_range = GST_DEBUG_FUNCPTR (acmdec_check_get_range);

	g_object_class_install_property(gobj_class, PROP_LOCATION,
		g_param_spec_string("location", "File location",
				    "Location to the file to read",
				    NULL, G_PARAM_READWRITE));
}

static void acmdec_base_init(gpointer klass_arg)
{
	AcmDecClass *klass = ACMDEC_CLASS(klass_arg);
       	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

	gst_element_class_add_pad_template(element_class,
			gst_static_pad_template_get(&src_factory));
	gst_element_class_set_details(element_class, &acmdec_details);
}

/*
 * File type handler.
 */

static void acmdec_detect_file(GstTypeFind *find, gpointer junk)
{
	guint8 *buf;
	static const guint8 acm_id[] = { 0x97, 0x28, 0x03, 0x01 };

	buf = gst_type_find_peek(find, 0, 4);
	if (!buf || memcmp(buf, acm_id, 4))
		return;

	gst_type_find_suggest(find, GST_TYPE_FIND_MAXIMUM,
			      gst_caps_new_simple ("audio/x-acm", NULL));
}

