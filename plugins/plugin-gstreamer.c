/*
 * plugin-gstreamer.c - Decoder plugin for GStreamer.
 *
 * Copyright (C) 2005  Marko Kreen
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
#include <gst/gstplugin.h>
#include <gst/bytestream/bytestream.h>

#include "libacm.h"

#define REQLEN (4 * 1024)

#if !defined(BYTE_ORDER) || !defined(LITTLE_ENDIAN)
#error BYTE_ORDER must be defined.
#endif

#if BYTE_ORDER == LITTLE_ENDIAN
#define ACM_NATIVE_BE 0
#else
#define ACM_NATIVE_BE 1
#endif

typedef struct AcmDec {
	GstElement element;
	GstPad *sinkpad, *srcpad;

	ACMStream *ctx;
	int seek_to; /* sample nr or -1 */
	int flush_pending;
	GstByteStream *bs;
} AcmDec;

typedef struct AcmDecClass {
	GstElementClass parent_class;
} AcmDecClass;


static GType acmdec_get_type(void);

#define TYPE_ACMDEC        acmdec_get_type()
#define ACMDEC(o)          G_TYPE_CHECK_INSTANCE_CAST((o),TYPE_ACMDEC,AcmDec)
#define ACMDEC_CLASS(k)    G_TYPE_CHECK_CLASS_CAST((k),TYPE_ACMDEC,AcmDec)
#define IS_ACMDEC(o)       G_TYPE_CHECK_INSTANCE_TYPE((o),TYPE_ACMDEC)
#define IS_ACMDEC_CLASS(c) G_TYPE_CHECK_CLASS_TYPE((c),TYPE_ACMDEC)

static GstElementClass *parent_class = NULL;

static GstStaticPadTemplate sink_factory =
GST_STATIC_PAD_TEMPLATE(
	"sink",	GST_PAD_SINK, GST_PAD_ALWAYS,
	GST_STATIC_CAPS("audio/x-acm")
);

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

static GstElementDetails acmdec_details = {
	"acmdec",
	"Codec/Decoder/Audio",
	"InterPlay ACM Audio decoder",
	"<markokr@gmail.com>"
};

static void send_discont(AcmDec *acm)
{
	GstFormat fmt;
	GstEvent *event;
	gint64 time = 64, bytes = 64, samples = 0;
	if (acm->ctx) {
		const ACMInfo *inf = acm_info(acm->ctx);
		samples = acm_pcm_total(acm->ctx) / inf->channels;
		
		fmt = GST_FORMAT_TIME;
		gst_pad_convert(acm->srcpad, GST_FORMAT_DEFAULT, samples,
				&fmt, &time);
		
		fmt = GST_FORMAT_BYTES;
		gst_pad_convert(acm->srcpad, GST_FORMAT_DEFAULT, samples,
				&fmt, &bytes);
	}
	event = gst_event_new_discontinuous(FALSE,
			GST_FORMAT_TIME, time,
			GST_FORMAT_BYTES, bytes,
			GST_FORMAT_DEFAULT, samples,
			NULL);
	gst_pad_push(acm->srcpad, GST_DATA(event));
}

static int handle_read_event(AcmDec *acm, GstEvent *event)
{
	int retry = 0;

	if (!event) {
		GST_ELEMENT_ERROR(acm, RESOURCE, READ, (NULL), (NULL));
		return 0;
	}

	switch (GST_EVENT_TYPE(event)) {
	case GST_EVENT_INTERRUPT:
	case GST_EVENT_EOS:
		gst_event_unref(event);
		break;
	case GST_EVENT_DISCONTINUOUS:
		send_discont(acm);
	case GST_EVENT_FLUSH:
		gst_event_unref(event);
		retry = 1;
		break;
	default:
		gst_pad_event_default(acm->srcpad, event);
		break;
	}
	return retry;
}

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

static void acmdec_stream_close(AcmDec *acm)
{
	if (acm->ctx) {
		acm_close(acm->ctx);
		acm->ctx = NULL;
	}
	if (acm->bs) {
		gst_bytestream_destroy(acm->bs);
		acm->bs = NULL;
	}
}

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

static void acmdec_obj_init(AcmDec *acm)
{
      	GST_FLAG_SET(acm, GST_ELEMENT_EVENT_AWARE);

	/* init sinkpad */
	acm->sinkpad = gst_pad_new_from_template(
			gst_static_pad_template_get(&sink_factory), "sink");
	gst_element_add_pad(GST_ELEMENT(acm), acm->sinkpad);
	
	/* init srcpad */
	acm->srcpad = gst_pad_new_from_template(
			gst_static_pad_template_get(&src_factory), "src");
	/* seeking support */
	gst_pad_set_event_function(acm->srcpad, acmdec_src_event);
	gst_pad_set_event_mask_function(acm->srcpad, acmdec_get_event_masks);
	/* stream info */
	gst_pad_set_query_function(acm->srcpad, acmdec_src_query);
 	gst_pad_set_query_type_function(acm->srcpad, acmdec_get_query_types);
	/* info val conversion */
	gst_pad_set_formats_function(acm->srcpad, acmdec_get_formats);
	gst_pad_set_convert_function(acm->srcpad, acmdec_src_convert);
	/* promise to set caps later */
	gst_pad_use_explicit_caps(acm->srcpad);
	/* add it */
	gst_element_add_pad(GST_ELEMENT(acm), acm->srcpad);

	/* set decoder loop */
	gst_element_set_loop_function(GST_ELEMENT(acm), acmdec_loop);

	acm->ctx = NULL;
	acm->seek_to = -1;
	acm->bs = NULL;
	acm->flush_pending = 0;
}

static void acmdec_dispose(GObject *obj)
{
	AcmDec *acm = ACMDEC(obj);

	acmdec_stream_close(acm);

	G_OBJECT_CLASS(parent_class)->dispose(obj);
}

static void acmdec_class_init(AcmDecClass *klass)
{
	parent_class = g_type_class_ref(GST_TYPE_ELEMENT);
	
	G_OBJECT_CLASS(klass)->dispose = acmdec_dispose;
	
	GST_ELEMENT_CLASS(klass)->change_state = acmdec_change_state;
}

static void acmdec_base_init(AcmDecClass *klass)
{
       	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
	gst_element_class_add_pad_template(element_class,
			gst_static_pad_template_get(&src_factory));
	gst_element_class_add_pad_template(element_class,
			gst_static_pad_template_get(&sink_factory));
	gst_element_class_set_details(element_class, &acmdec_details);
}

static const GTypeInfo acmdec_class_info = {
	sizeof(AcmDecClass),			/* class_size */
	(GBaseInitFunc)acmdec_base_init,	/* base_init */
	NULL,					/* base_finalize */
	(GClassInitFunc) acmdec_class_init,	/* class_init */
	NULL,					/* class_finalize */
	NULL,					/* class_data */
	sizeof(AcmDec),				/* instance_size */
	0,					/* n_prealloc */
	(GInstanceInitFunc)acmdec_obj_init,	/* instance_init */
};

static GType acmdec_get_type(void)
{
	static GType code = 0;
    	if (!code)
		code = g_type_register_static(GST_TYPE_ELEMENT,
			"GstAcmDec", &acmdec_class_info, 0);
	return code;
}

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

static gboolean acmdec_plugin_init(GstPlugin *plugin)
{
	static char *ext_list[] = {"acm", NULL};
	gboolean res;
	GstCaps *caps;

	if (!gst_library_load("gstbytestream"))
		return FALSE;

	res = gst_element_register(plugin, "acmdec",
				   GST_RANK_PRIMARY, TYPE_ACMDEC);
	if (!res)
		return FALSE;

	caps = gst_caps_new_simple("audio/x-acm", NULL);
	res = gst_type_find_register(plugin, "audio/x-acm", GST_RANK_PRIMARY,
			acmdec_detect_file, ext_list, caps, NULL);
	
	return res;
}

GST_PLUGIN_DEFINE(
	GST_VERSION_MAJOR, GST_VERSION_MINOR,
	"acmdec",
	"InterPlay ACM Audio Format",
	acmdec_plugin_init,
	LIBACM_VERSION,
	"GPL",
	"libacm",
	"http://libacm.berlios.de/"
);

