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
#include <gst/base/gstpushsrc.h>

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
	GstPushSrc pushsrc;

	gchar *location;
	ACMStream *ctx;
	int seek_to; /* sample nr or -1 */
	int flush_pending;
} AcmDec;

typedef struct AcmDecClass {
	GstPushSrcClass parent_class;
} AcmDecClass;

/*
 * define element class
 */

#define TYPE_ACMDEC        (acmdec_get_type())
#define ACMDEC(o)          G_TYPE_CHECK_INSTANCE_CAST((o),TYPE_ACMDEC,AcmDec)
#define ACMDEC_CLASS(k)    G_TYPE_CHECK_CLASS_CAST((k),TYPE_ACMDEC,AcmDecClass)
#define IS_ACMDEC(o)       G_TYPE_CHECK_INSTANCE_TYPE((o),TYPE_ACMDEC)
#define IS_ACMDEC_CLASS(c) G_TYPE_CHECK_CLASS_TYPE((c),TYPE_ACMDEC)

GST_BOILERPLATE (AcmDec, acmdec, GstPushSrc, GST_TYPE_PUSH_SRC);

static GstElementDetails acmdec_details = {
	"acmdec",
	"Codec/Decoder/Audio",
	"InterPlay ACM Audio decoder",
	"Marko Kreen <markokr@gmail.com>"
};

/*
 * define plugin
 */

static gboolean acmdec_plugin_init(GstPlugin *plugin)
{
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

#if 0
static int acmio_read(void *dst, int size, int n, void *arg)
{
	int res;
	AcmDec *acm = ACMDEC(arg);
	GstEvent *event;
	guint32 avail;
	guint8 *buf = NULL;
	int reqlen = size * n;
	GstByteStream *bs = acm->bs;
retry:
	res = gst_bytestream_peek_bytes(bs, &buf, reqlen);
	if (res != reqlen) {
		gst_bytestream_get_status(acm->bs, &avail, &event);
		if (handle_read_event(acm, event))
			goto retry;
		if (avail > 0) {
			reqlen = avail;
			goto retry;
		}
	} else {
		memcpy(dst, buf, res);
		gst_bytestream_flush_fast(bs, res);
	}
	return res;
}
#endif

#if 0
static int acmio_seek(void *arg, int offset, int whence)
{
	int res, tmp;
	AcmDec *acm = ACMDEC(arg);
	GstSeekType gst_whence;
	if (whence == SEEK_SET)
		gst_whence = GST_SEEK_METHOD_SET;
	else if (whence == SEEK_CUR)
		gst_whence = GST_SEEK_METHOD_CUR;
	else
		gst_whence = GST_SEEK_METHOD_END;
	res = gst_bytestream_seek(acm->bs, offset, gst_whence);
	return res ? 0 : -1;
}

static int acmio_get_length(void *arg)
{
	AcmDec *acm = ACMDEC(arg);
	return gst_bytestream_length(acm->bs);
}
#endif

#if 0
static gboolean acmdec_stream_init(AcmDec *acm)
{
	int res;
	GstCaps *caps;
	const ACMInfo *inf;
	static const acm_io_callbacks iofuncs = {
		acmio_read,
		acmio_seek,
		NULL,
		acmio_get_length
	};
	res = acm_open_decoder(&acm->ctx, acm, iofuncs);
	if (res < 0)
		return FALSE;

	inf = acm_info(acm->ctx);
	caps = gst_caps_from_string(BASE_CAPS);
	gst_caps_set_simple(caps,
			"channels", G_TYPE_INT, inf->channels,
			"rate", G_TYPE_INT, inf->rate, NULL);
	res = gst_pad_set_explicit_caps(acm->srcpad, caps);
	if (!res)
		GST_ELEMENT_ERROR(acm, CORE, NEGOTIATION, (NULL), (NULL));

	gst_caps_free(caps);
	return res;
}
#endif

#if 0
static void handle_seek(AcmDec *acm)
{
	int res;
	GstEvent *ev;

	res = acm_seek_pcm(acm->ctx, acm->seek_to);
	if (res >= 0) {
		if (acm->flush_pending) {
			ev = gst_event_new(GST_EVENT_FLUSH);
			gst_pad_push(acm->srcpad, GST_DATA(ev));
		}
		send_discont(acm);
	}
	acm->flush_pending = 0;
	acm->seek_to = -1;
}
#endif

#if 0
static void acmdec_loop(GstElement *elem)
{
	AcmDec *acm = ACMDEC(elem);
	const ACMInfo *inf = acm_info(acm->ctx);
	GstBuffer *buf;
	GstFormat fmt = GST_FORMAT_TIME;
	int res;
	char *buf_ptr;
	gint64 buf_pos, buf_dur;
	gint64 test_pos, test_dur;

	if (!GST_PAD_IS_USABLE(acm->srcpad))
		return;

	if (!acm->ctx)
		if (!acmdec_stream_init(acm))
			return;

	if (acm->seek_to >= 0)
		handle_seek(acm);

	/* get current pos */
	gst_pad_query(acm->srcpad, GST_QUERY_POSITION, &fmt, &buf_pos);

	/* read data */
	buf = gst_buffer_new_and_alloc(REQLEN);
	buf_ptr = (char*)GST_BUFFER_DATA(buf);
	res = acm_read_loop(acm->ctx, buf_ptr, REQLEN, ACM_NATIVE_BE, 2, 1);
	if (res <= 0) {
		gst_buffer_unref(buf);
#if 0
		if (res < 0) {
			/* GST_ERROR_OBJECT(acm, "Decode failed");
			   return; */
			printf("got eof err: total_pcm=%d cur_pcm=%d\n",
					acm_pcm_total(acm->ctx),
					acm_pcm_tell(acm->ctx)
					);
		}
#endif
		GstEvent *ev = gst_event_new(GST_EVENT_EOS);
		gst_element_set_eos(elem);
		gst_pad_push(acm->srcpad, GST_DATA(ev));
		return;
	}

	gst_pad_convert(acm->srcpad, GST_FORMAT_BYTES, res, &fmt, &buf_dur);

	GST_BUFFER_SIZE(buf) = res;
	GST_BUFFER_TIMESTAMP(buf) = buf_pos;
	GST_BUFFER_DURATION(buf) = buf_dur;

	gst_pad_push(acm->srcpad, GST_DATA(buf));
}
#endif

#if 0
static GstElementStateReturn acmdec_change_state(GstElement *elem)
{
	AcmDec *acm = ACMDEC(elem);
	guint32 avail;
	GstEvent *event;

	switch (GST_STATE_TRANSITION(elem)) {
	case GST_STATE_READY_TO_PAUSED:
		acm->bs = gst_bytestream_new(acm->sinkpad);
		break;
	case GST_STATE_PAUSED_TO_READY:
		acmdec_stream_close(acm);
		break;
	default:
		break;
	}

	if (GST_ELEMENT_CLASS(parent_class)->change_state)
		return GST_ELEMENT_CLASS(parent_class)->change_state(elem);

	return GST_STATE_SUCCESS;	
}
#endif

#if 0
#define SEEK_FLAGS ( \
	GST_SEEK_METHOD_SET | GST_SEEK_METHOD_CUR | GST_SEEK_METHOD_END | \
	GST_SEEK_FLAG_ACCURATE | GST_SEEK_FLAG_FLUSH )

static const GstEventMask *acmdec_get_event_masks(GstPad *pad)
{
	static const GstEventMask event_masks[] = {
		{GST_EVENT_SEEK, (GstEventFlag)SEEK_FLAGS},
		{(GstEventType)0, (GstEventFlag)0}
	};
    	return event_masks;
}

static gboolean acmdec_src_event(GstPad *pad, GstEvent *event)
{
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad));
	gboolean res = FALSE;
	GstFormat fmt = GST_FORMAT_DEFAULT;
	gint64 tmp_pos, cur_pos;

	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_SEEK:
		res = gst_pad_convert(acm->srcpad, 
					GST_EVENT_SEEK_FORMAT(event),
					GST_EVENT_SEEK_OFFSET(event),
					&fmt, &tmp_pos);
		if (!res)
			break;

		cur_pos = acm_pcm_tell(acm->ctx);
		switch (GST_EVENT_SEEK_METHOD(event)) {
		case GST_SEEK_METHOD_SET:
			break;
		case GST_SEEK_METHOD_CUR:
			tmp_pos += cur_pos;
			break;
		case GST_SEEK_METHOD_END:
			tmp_pos = acm_pcm_total(acm->ctx) - tmp_pos;
			break;
		default:
			res = FALSE;
			goto out;
		}

		acm->seek_to = tmp_pos;
		if (GST_EVENT_SEEK_FLAGS(event) & GST_SEEK_FLAG_FLUSH)
			acm->flush_pending = 1;
		else
			acm->flush_pending = 0;
		break;
	}
out:
	gst_event_unref(event);
	return res;
}
#endif

#if 0
static const GstFormat *acmdec_get_formats(GstPad *pad)
{
	static const GstFormat formats[] = {
		GST_FORMAT_BYTES,
		GST_FORMAT_DEFAULT,
		GST_FORMAT_TIME,
		(GstFormat) 0
	};
	return formats;
}

static gboolean acmdec_src_convert(GstPad *pad,
			GstFormat src_format, gint64 src_value,
			GstFormat *dest_format, gint64 *dest_value)
{
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad));
	gboolean res = TRUE;
	const ACMInfo *inf;
	int bps;

	if (!acm->ctx)
		return FALSE;

	inf = acm_info(acm->ctx);
	bps = inf->channels * ACM_WORD;
	switch (src_format) {
	case GST_FORMAT_DEFAULT:
		switch (*dest_format) {
		case GST_FORMAT_TIME:
			*dest_value = src_value * GST_SECOND / inf->rate;
			break;
		case GST_FORMAT_BYTES:
			*dest_value = src_value * bps;
			break;
		default:
			res = FALSE;
			break;
		}
		break;

	case GST_FORMAT_TIME:
		switch (*dest_format) {
		case GST_FORMAT_DEFAULT:
			*dest_value = src_value * inf->rate / GST_SECOND;
			break;
		case GST_FORMAT_BYTES:
			*dest_value = src_value * inf->rate * bps / GST_SECOND;
			break;
		default:
			res = FALSE;
			break;
		}
		break;
	case GST_FORMAT_BYTES:
		switch (*dest_format) {
		case GST_FORMAT_DEFAULT:
			*dest_value = src_value / bps;
			break;
		case GST_FORMAT_TIME:
			*dest_value = src_value * GST_SECOND / (bps*inf->rate);
			break;
		default:
			res = FALSE;
			break;
		}
		break;

	default:
		res = FALSE;
		break;
	}

	return TRUE;
}
#endif

#if 0
/*
 * Query stuff.
 */

static const GstQueryType *acmdec_get_query_types(GstPad *pad)
{
	static const GstQueryType query_types[] = {
		GST_QUERY_TOTAL,
		GST_QUERY_POSITION,
		(GstQueryType) 0
	};
	return query_types;
}

static gboolean acmdec_src_query(GstPad *pad, GstQueryType type,
				 GstFormat *format, gint64 *value)
{
  	gint64 srcval;
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad));

	if (!acm->ctx)
		return FALSE;

	switch (type) {
	case GST_QUERY_TOTAL:
		srcval = acm_pcm_total(acm->ctx);
		break;
	case GST_QUERY_POSITION:
		if (acm->seek_to >= 0)
			srcval = acm->seek_to;
		else
			srcval = acm_pcm_tell(acm->ctx);
		break;
	default:
		return FALSE;
	}

	return gst_pad_convert(pad, GST_FORMAT_DEFAULT, srcval, format, value);
}
#endif

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

static GstFlowReturn acmdec_create(GstPushSrc *psrc, GstBuffer **buf)
{
	AcmDec *acm = ACMDEC(psrc);
	GstBaseSrc *base = GST_BASE_SRC(acm);
	void *data;

	int need_bytes, blocksize, got;

	GST_OBJECT_LOCK (base);
	blocksize = base->blocksize;
	GST_OBJECT_UNLOCK (base);

	need_bytes = blocksize;

	*buf = gst_buffer_new_and_alloc (need_bytes);
	data = GST_BUFFER_DATA (*buf);
	GST_BUFFER_SIZE (*buf) = 0;

	got = acm_read(acm->ctx, data, need_bytes, ACM_NATIVE_BE, 2, 1);
	if (got <= 0) {
		gst_buffer_unref (*buf);
		*buf = NULL;
		return GST_FLOW_UNEXPECTED;
	}
	GST_BUFFER_SIZE (*buf) = got;

	gst_buffer_set_caps (*buf, GST_PAD_CAPS (GST_BASE_SRC_PAD (acm)));

	return GST_FLOW_OK;
}

/*
 * Object initialization
 */

static void acmdec_init(AcmDec *acm, AcmDecClass *klass)
{
	GstBaseSrc *base = GST_BASE_SRC(acm);

	acm->location = NULL;
	acm->ctx = NULL;
	acm->seek_to = -1;
	acm->flush_pending = 0;

	gst_pad_set_query_function (base->srcpad, GST_DEBUG_FUNCPTR (acmdec_src_query));
	gst_pad_set_query_type_function (base->srcpad, GST_DEBUG_FUNCPTR (acmdec_get_query_types));
	base->blocksize = 2048; // ??
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
	GstPushSrcClass *push_class = GST_PUSH_SRC_CLASS(acm_class);
	
	gobj_class->finalize = GST_DEBUG_FUNCPTR (acmdec_finalize);
	gobj_class->set_property = GST_DEBUG_FUNCPTR (acmdec_set_property);
	gobj_class->get_property = GST_DEBUG_FUNCPTR (acmdec_get_property);
	
	bsrc_class->start = GST_DEBUG_FUNCPTR (acmdec_start);
	bsrc_class->stop = GST_DEBUG_FUNCPTR (acmdec_stop);

	push_class->create = GST_DEBUG_FUNCPTR (acmdec_create);

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

#if 0
static void acmdec_detect_file(GstTypeFind *find, gpointer junk)
{
	guint8 *buf;
	const GstCaps *caps;
	static const guint8 acm_id[] = { 0x97, 0x28, 0x03 };
	static GstStaticCaps __caps = GST_STATIC_CAPS("audio/x-acm");

	buf = gst_type_find_peek(find, 0, 3);
	if (!buf || memcmp(buf, acm_id, 3))
		return;

	caps = gst_static_caps_get(&__caps);
	gst_type_find_suggest(find, GST_TYPE_FIND_MAXIMUM, caps);
}
#endif

