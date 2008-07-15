/*
 * libacm plugin for GStreamer
 *
 * Copyright (C) 2004-2008  Marko Kreen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sys/param.h>
#include <string.h>

#include <gst/gst.h>
#include <gst/base/gstadapter.h>

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

#define ull unsigned long long


#define FN(f) GST_DEBUG_FUNCPTR(f)

GST_DEBUG_CATEGORY_STATIC (acmdec_debug);
#define GST_CAT_DEFAULT acmdec_debug


/*
 * AcmDec struct
 */
typedef struct AcmDec {
	GstElement parent;

	GstPad *srcpad, *sinkpad;
	ACMStream *ctx;

	GstAdapter *adapter;

	int fileofs;
	gboolean discont;

	int seek_to;      /* in pcm samples */
	gboolean restart; /* if the stream should be rewinded */
} AcmDec;

typedef struct AcmDecClass {
	GstElementClass parent_class;
} AcmDecClass;

/*
 * define element class
 */

#define TYPE_ACMDEC        (acmdec_get_type())
#define ACMDEC(o)          G_TYPE_CHECK_INSTANCE_CAST((o),TYPE_ACMDEC,AcmDec)
#define ACMDEC_CLASS(k)    G_TYPE_CHECK_CLASS_CAST((k),TYPE_ACMDEC,AcmDecClass)
#define IS_ACMDEC(o)       G_TYPE_CHECK_INSTANCE_TYPE((o),TYPE_ACMDEC)
#define IS_ACMDEC_CLASS(c) G_TYPE_CHECK_CLASS_TYPE((c),TYPE_ACMDEC)

GST_BOILERPLATE (AcmDec, acmdec, GstElement, GST_TYPE_ELEMENT);

static GstElementDetails acmdec_details = GST_ELEMENT_DETAILS(
	"acmdec",
	"Codec/Decoder/Audio",
	"InterPlay ACM Audio decoder",
	"Marko Kreen <markokr@gmail.com>"
);

/*
 * define plugin
 */

static void acmdec_detect_file(GstTypeFind *find, gpointer junk);

static gboolean acmdec_plugin_init(GstPlugin *plugin)
{
	static char *ext_list[] = {"acm", NULL};
	GstCaps *caps = gst_caps_new_simple("audio/x-acm", NULL);

	if (!gst_element_register(plugin, "acmdec", GST_RANK_PRIMARY, TYPE_ACMDEC))
		return FALSE;

	if (!gst_type_find_register(plugin, "type_acm", GST_RANK_PRIMARY,
			      	    acmdec_detect_file, ext_list, caps, NULL, NULL))
		return FALSE;

	GST_DEBUG_CATEGORY_INIT (acmdec_debug, "acmdec", 0, "ACM decoder");
	return TRUE;
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

static GstStaticPadTemplate acmdec_src_template =
GST_STATIC_PAD_TEMPLATE(
	"src", GST_PAD_SRC, GST_PAD_ALWAYS,
	GST_STATIC_CAPS(BASE_CAPS ", "
			"rate = (int) [ 4000, 96000 ], "
			"channels = (int) [ 1, 2 ]")
);

static GstStaticPadTemplate acmdec_sink_template =
GST_STATIC_PAD_TEMPLATE (
	"sink", GST_PAD_SINK, GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/x-acm"));

/*
 * Fire reader with gst_pad_pull_range()
 */

static int acmdec_chain_read(void *dst, int size, int n, void *arg)
{
	unsigned int realsize = size * n;
	AcmDec *acm = arg;
	const void *data;

	realsize = MIN(gst_adapter_available(acm->adapter), realsize);
	if (realsize == 0) {
		GST_DEBUG_OBJECT(acm, "got eof?");
		return 0;
	}

	data = gst_adapter_peek(acm->adapter, realsize);
	memcpy(dst, data, realsize);
	gst_adapter_flush(acm->adapter, realsize);
	acm->fileofs += realsize;

	return realsize;
}

static int acmdec_chain_seek(void *arg, int ofs, int whence)
{
	AcmDec *acm = arg;
	int fileofs = acm->fileofs;
	GstEvent *ev;
	int res = -1;
	GstPad *peer;

	switch (whence) {
	case SEEK_SET:
		fileofs = ofs;
		break;
	case SEEK_CUR:
		fileofs += ofs;
		break;
	case SEEK_END:
		/* unsupported */
		return -1;
	}

	/* do seek on sink pad */
	peer = gst_pad_get_peer(acm->sinkpad);
	if (!peer)
		return -1;
	ev = gst_event_new_seek(1.0, GST_FORMAT_BYTES,
				GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
				GST_SEEK_TYPE_SET, fileofs, 0, -1);
	if (gst_pad_send_event(peer, ev)) {
		acm->fileofs = fileofs;
		res = fileofs;
	}
	gst_object_unref (peer);

	return res;
}

static int acmdec_io_get_size(void *arg)
{
	GstFormat fmt = GST_FORMAT_BYTES;
	gint64 len;
	AcmDec *acm = arg;
	GstPad *peer;
	gboolean ok;
	
	peer = gst_pad_get_peer(acm->sinkpad);
	if (!peer)
		return -1;
	ok = gst_pad_query_duration (peer, &fmt, &len);
	gst_object_unref (peer);

	if (!ok || fmt != GST_FORMAT_BYTES || len <= 0)
		return -1;
	return len;
}

/*
 * Object init/cleanup
 */

static void acmdec_post_tags(AcmDec *acm)
{
	GstTagList *tags;
	GstMessage *msg;
	GST_DEBUG_OBJECT(acm, "sending tags");
	tags = gst_tag_list_new();
	gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE,
			 GST_TAG_AUDIO_CODEC, "ACM", NULL);
	gst_tag_list_add(tags, GST_TAG_MERGE_REPLACE,
			 GST_TAG_BITRATE, acm_bitrate(acm->ctx), NULL);
	msg = gst_message_new_tag (GST_OBJECT (acm), tags);
	if (!gst_element_post_message (GST_ELEMENT (acm), msg))
		GST_ERROR_OBJECT(acm, "failed to send tags");
}

static gboolean acmdec_init_decoder(AcmDec *acm)
{
	static const acm_io_callbacks chain_cb = {
		.read_func = acmdec_chain_read,
		.seek_func = acmdec_chain_seek,
		.get_length_func = acmdec_io_get_size,
	};
	int res;
	GstCaps *caps;

	GST_DEBUG_OBJECT(acm, "init decoder");
	res = acm_open_decoder(&acm->ctx, acm, chain_cb);
	if (res < 0) {
		GST_DEBUG_OBJECT(acm, "decoder init failed: %s", acm_strerror(res));
		return FALSE;
	}
	GST_DEBUG_OBJECT(acm, "size=%d samples=%d", acm->ctx->data_len, acm->ctx->total_values);

	caps = gst_caps_from_string(BASE_CAPS);
	gst_caps_set_simple(caps,
			    "channels", G_TYPE_INT, acm_channels(acm->ctx),
			    "rate", G_TYPE_INT, acm_rate(acm->ctx), NULL);
	gst_pad_use_fixed_caps(acm->srcpad);
	if (!gst_pad_set_caps(acm->srcpad, caps)) {
		gst_caps_unref(caps);
		GST_ERROR_OBJECT(acm, "failed to set caps");
		return FALSE;
	}
	gst_caps_unref(caps);

	acmdec_post_tags(acm);

	return TRUE;
}

static void acmdec_reset(AcmDec *acm)
{
	GST_DEBUG_OBJECT(acm, "do reset");
	if (acm->ctx) {
		acm_close(acm->ctx);
		acm->ctx = NULL;
	}
	if (acm->adapter) {
		g_object_unref(acm->adapter);
		acm->adapter = NULL;
	}
	acm->fileofs = 0;
	acm->discont = FALSE;
	acm->seek_to = -1;
	acm->restart = FALSE;
}

static void acmdec_dispose(GObject *obj)
{
	AcmDec *acm = ACMDEC(obj);

	acmdec_reset(acm);

	G_OBJECT_CLASS (parent_class)->dispose (obj);
}


static gboolean acmdec_convert(AcmDec *acm, GstFormat src_format, gint64 src_value,
			       GstFormat *dest_format, gint64 *dest_value)
{
	gboolean res = TRUE;
	guint64 pcmval;

	if (src_format == *dest_format || src_value <= 0) {
		*dest_value = src_value;
		return TRUE;
	}

	if (!acm->ctx)
		goto no_stream;

	switch (src_format) {
	case GST_FORMAT_TIME:
		pcmval = src_value * acm_rate(acm->ctx) / GST_SECOND;
		break;
	case GST_FORMAT_BYTES:
		pcmval = src_value / (2 * acm_channels(acm->ctx));
		break;
	case GST_FORMAT_DEFAULT:
		pcmval = src_value;
		break;
	default:
		goto no_format;
	}

	switch (*dest_format) {
	case GST_FORMAT_BYTES:
		*dest_value = pcmval * 2 * acm_channels(acm->ctx);
		break;
	case GST_FORMAT_DEFAULT:
		*dest_value = pcmval;
		break;
	case GST_FORMAT_TIME:
		*dest_value = GST_SECOND * pcmval / acm_rate(acm->ctx);
		break;
	default:
		goto no_stream;
	}

done:
	return res;

no_stream:
	GST_DEBUG_OBJECT(acm, "no stream open");
	res = FALSE;
	goto done;
no_format:
	GST_DEBUG_OBJECT(acm, "formats unsupported");
	res = FALSE;
	goto done;
}

/*
 * srcpad methods
 */

static const GstQueryType *
acmdec_src_query_types (GstPad *pad)
{
	static const GstQueryType speex_dec_src_query_types[] = {
		GST_QUERY_POSITION, GST_QUERY_DURATION, GST_QUERY_CONVERT, 0 };
    	return speex_dec_src_query_types;
}


static gboolean
acmdec_src_query (GstPad *pad, GstQuery *query)
{
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad)); /* incref */
	gboolean res = FALSE;
	gint64 pcmval, val, dest_val;
	GstFormat fmt = GST_FORMAT_TIME, dest_fmt = 0;

	//GST_DEBUG_OBJECT(acm, "src_query: %s", GST_QUERY_TYPE_NAME (query));

	switch (GST_QUERY_TYPE (query)) {
	case GST_QUERY_POSITION:
		gst_query_parse_position (query, &fmt, NULL);
		if (acm->seek_to >= 0)
			pcmval = acm->seek_to;
		else
			pcmval = acm_pcm_tell(acm->ctx);
		res = acmdec_convert(acm, GST_FORMAT_DEFAULT, pcmval, &fmt, &val);
		if (res)
			gst_query_set_position(query, fmt, val);
		break;
	case GST_QUERY_DURATION:
		gst_query_parse_duration (query, &fmt, NULL);
		pcmval = acm_pcm_total(acm->ctx);
		res = acmdec_convert(acm, GST_FORMAT_DEFAULT, pcmval, &fmt, &val);
		if (res)
			gst_query_set_duration(query, fmt, val);
		break;
	case GST_QUERY_CONVERT:
		gst_query_parse_convert (query, &fmt, &val, &dest_fmt, NULL);
		res = acmdec_convert(acm, fmt, val, &dest_fmt, &dest_val);
		if (res)
			gst_query_set_convert(query, fmt, val, dest_fmt, dest_val);
		break;
	default:
		res = gst_pad_query_default (pad, query);
		break;
	}

	gst_object_unref (acm);
	return res;
}

static gboolean do_seek(AcmDec *acm, GstEvent *event)
{
	GstFormat format, tformat;
	gdouble rate;
	GstSeekFlags flags;
	GstSeekType cur_type, stop_type;
	gint64 cur, stop;
	gint64 tstop;

	gst_event_parse_seek (event, &rate, &format, &flags, &cur_type, &cur,
			      &stop_type, &stop);
	GST_DEBUG_OBJECT(acm, "do_seek: rate=%0.1f format=%d seek_flags=%d"
			 " curtype=%d stoptype=%d curpos=%lld stop_pos=%lld", 
			 rate, format, flags, cur_type, stop_type, (long long)cur, (long long)stop);
	if (format == GST_FORMAT_TIME) {
		GST_DEBUG_OBJECT(acm, "fmt as time=%" GST_TIME_FORMAT, GST_TIME_ARGS(cur));
	}

	if (flags & GST_SEEK_FLAG_SEGMENT) {
		GST_WARNING_OBJECT(acm, "GST_SEEK_FLAG_SEGMENT not supported");
		return FALSE;
	}
	if (!(flags & GST_SEEK_FLAG_FLUSH)) {
		GST_WARNING_OBJECT(acm, "!GST_SEEK_FLAG_FLUSH not supported");
		return FALSE;
	}
	if (stop_type) {
		GST_WARNING_OBJECT(acm, "seek with stop_pos not supported");
		return FALSE;
	}
	if (cur_type != GST_SEEK_TYPE_SET) {
		GST_WARNING_OBJECT(acm, "seek with cur_type = CUR/END not supported");
		return FALSE;
	}

	tformat = GST_FORMAT_DEFAULT;
	if (!acmdec_convert (acm, format, cur, &tformat, &tstop))
		return FALSE;

	GST_DEBUG_OBJECT(acm, "do_seek: newpos=%llu curpos=%llu", tstop, (ull)acm_pcm_tell(acm->ctx));

	/*
	 * Set seek pos.  Lock as touched from play thread too.
	 */

	GST_OBJECT_LOCK(acm);
	if (tstop <= acm_pcm_tell(acm->ctx))
		acm->restart = TRUE;
	acm->seek_to = tstop;
	GST_OBJECT_UNLOCK(acm);

	return TRUE;
}

static gboolean handle_sink_newsegment(AcmDec *acm, GstEvent *event)
{
	GstFormat fmt;
	gboolean is_update;
	gint64 start, end, base;
	gdouble rate;

	gst_event_parse_new_segment (event, &is_update, &rate, &fmt, &start,
				     &end, &base);

	GST_DEBUG_OBJECT(acm, "Sink Newsegment: fmt=%s upd=%d rate=%.2f"
			 " start=%llu end=%llu base=%llu",
			 gst_format_get_name(fmt),
			 is_update, rate, (ull)start, (ull)end, (ull)base);
	return TRUE;
}

static gboolean acmdec_src_event(GstPad *pad, GstEvent *event)
{
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad)); /* incref */
	gboolean res = FALSE;


	switch (GST_EVENT_TYPE (event)) {
	case GST_EVENT_SEEK:
		res = do_seek(acm, event);
		gst_event_unref(event);
		break;
	default:
		GST_DEBUG_OBJECT (acm, "src received %s event", GST_EVENT_TYPE_NAME (event));
	case GST_EVENT_NAVIGATION:
		res = gst_pad_event_default (pad, event);
		break;
	}
	gst_object_unref(acm);
	return res;
}

/*
 * change state
 */

static GstStateChangeReturn acmdec_change_state (GstElement *elem, GstStateChange transition)
{
	GstStateChangeReturn ret;
	AcmDec *acm = ACMDEC(elem);

	GST_DEBUG_OBJECT(acm, "change_state: from=%s to=%s",
			 gst_element_state_get_name(
				GST_STATE_TRANSITION_CURRENT(transition)),
			 gst_element_state_get_name(
				GST_STATE_TRANSITION_NEXT(transition)));

	switch (transition) {
	case GST_STATE_CHANGE_READY_TO_PAUSED:
	case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
	case GST_STATE_CHANGE_NULL_TO_READY:
	default:
		break;
	}

	ret = parent_class->change_state(elem, transition);
	if (ret != GST_STATE_CHANGE_SUCCESS) {
		GST_DEBUG_OBJECT(acm, "parent change_state: %s",
				 gst_element_state_change_return_get_name(ret));
		return ret;
	}

	switch (transition) {
	case GST_STATE_CHANGE_PAUSED_TO_READY:
		acmdec_reset(acm);
		break;
	case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
	case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
	case GST_STATE_CHANGE_READY_TO_NULL:
		break;
	default:
		break;
	}

	return ret;
}

static void acmdec_send_segment(AcmDec *acm)
{
	GstEvent *ev;
	guint64 timepos, start, stop;

	timepos = acm_time_tell(acm->ctx);
	start = GST_SECOND * timepos / 1000;
	stop = GST_SECOND * acm_time_total(acm->ctx) / 1000;

	ev = gst_event_new_new_segment(FALSE, 1.0,
				       GST_FORMAT_TIME, start, stop, start);
	gst_pad_push_event(acm->srcpad, ev);
}


static GstFlowReturn decode_block(AcmDec *acm, int size)
{
	GstBuffer *buf = NULL;
	GstFlowReturn flow;
	int got;
	int seek_to = acm->seek_to;
	gboolean restart;
	gint64 pcmpos;

	GST_OBJECT_LOCK(acm);
	seek_to = acm->seek_to;
	restart = acm->restart;
	GST_OBJECT_UNLOCK(acm);

	if (seek_to >= 0) {
		if (restart) {
			GST_LOG_OBJECT(acm, "doing stream rewind");
			gst_adapter_clear(acm->adapter);
			if (acm_seek_pcm(acm->ctx, 0) < 0) {
				GST_LOG_OBJECT(acm, "acm_seek_pcm failed");
				return GST_FLOW_UNEXPECTED;
			}

			GST_OBJECT_LOCK(acm);
			acm->restart = FALSE;
			acm->discont = TRUE;
			GST_OBJECT_UNLOCK(acm);

			return GST_FLOW_OK;
		}
		pcmpos = acm_pcm_tell(acm->ctx);
		if (pcmpos < seek_to) {
			got = acm_read_loop(acm->ctx, NULL, size, ACM_NATIVE_BE, 2, 1);
			if (got > 0)
				return GST_FLOW_OK;
			else
				return GST_FLOW_UNEXPECTED;
		}
		GST_DEBUG_OBJECT(acm, "reached seek pos at %d", (int)pcmpos);
		GST_OBJECT_LOCK(acm);
		acm->seek_to = -1;
		GST_OBJECT_UNLOCK(acm);
	}

	flow = gst_pad_alloc_buffer_and_set_caps(acm->srcpad, -1, size, GST_PAD_CAPS(acm->srcpad), &buf);
	if (flow != GST_FLOW_OK)
		goto error;

	if (acm->discont) {
		acmdec_send_segment(acm);

		buf = gst_buffer_make_metadata_writable(buf);
		GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DISCONT);
		acm->discont = FALSE;
	}

        got = acm_read_loop(acm->ctx, GST_BUFFER_DATA(buf), size, ACM_NATIVE_BE, 2, 1);
        if (got < 0) {
		flow = GST_FLOW_UNEXPECTED;
		goto error;
	} else if (got == 0) {
		gst_pad_push_event(acm->srcpad, gst_event_new_eos());
		goto error;
	}

        GST_BUFFER_SIZE (buf) = got;
	GST_BUFFER_TIMESTAMP (buf) = GST_CLOCK_TIME_NONE;
	GST_BUFFER_DURATION (buf) = GST_CLOCK_TIME_NONE;

        return gst_pad_push(acm->srcpad, buf);

error:
	if (buf)
		gst_buffer_unref(buf);
	return flow;
}

static gboolean acmdec_sink_event(GstPad *pad, GstEvent *event)
{
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad));
	gboolean res;
	GstFlowReturn flow;

	switch (GST_EVENT_TYPE (event)) {
	case GST_EVENT_NEWSEGMENT:
		res = handle_sink_newsegment(acm, event);
		break;
	case GST_EVENT_EOS:
		GST_DEBUG_OBJECT (acm, "Sink received %s event", GST_EVENT_TYPE_NAME (event));

		while (gst_adapter_available(acm->adapter) > 0) {
			int out_block = acm->ctx->block_len * ACM_WORD;
			flow = decode_block(acm, out_block);
			if (flow != GST_FLOW_OK)
				break;
		}
		res = TRUE;
		break;
	case GST_EVENT_FLUSH_STOP:
	default:
		GST_DEBUG_OBJECT (acm, "Sink received %s event", GST_EVENT_TYPE_NAME (event));
		res = gst_pad_event_default(pad, event);
		break;
	}
	gst_object_unref(acm);

	return res;
}

static GstFlowReturn acmdec_sink_chain(GstPad *pad, GstBuffer *buf)
{
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad)); /* incref */
	unsigned int out_block, in_block;
	GstFlowReturn flow = GST_FLOW_OK;

	if (0)
	GST_DEBUG_OBJECT(acm, "Input buffer: size=%u"
			 " tstamp=%llu"
			 " dur=%llu"
			 " ofs=%llu"
			 " ofs2=%llu",
			 buf->size, (ull)buf->timestamp, (ull)buf->duration,
			 (ull)buf->offset, (ull)buf->offset_end);

	if (!acm->adapter)
		acm->adapter = gst_adapter_new ();

	if (GST_BUFFER_IS_DISCONT(buf)) {
		GST_DEBUG_OBJECT(acm, "discont buffer");
		acm->discont = TRUE;
	}
	gst_adapter_push (acm->adapter, buf);
	buf = NULL;

	if (!acm->ctx) {
		if (!acmdec_init_decoder(acm)) {
			flow = GST_FLOW_UNEXPECTED;
			goto out;
		}
	}

	out_block = acm->ctx->block_len * ACM_WORD;
	in_block = out_block / 2;

	while (flow == GST_FLOW_OK) {
		if (gst_adapter_available(acm->adapter) < in_block)
			break;
		flow = decode_block(acm, out_block);
	}

out:
	gst_object_unref(acm);
	return flow;
}

/*
 * Object initialization
 */

static void acmdec_init(AcmDec *acm, AcmDecClass *klass)
{
	GstPad *src, *sink;

	GST_DEBUG_OBJECT(acm, "acmdec_init");

	acmdec_reset(acm);

	sink = gst_pad_new_from_static_template (&acmdec_sink_template, "sink");
	gst_pad_set_event_function(sink, FN(acmdec_sink_event));
	gst_pad_set_chain_function (sink, FN(acmdec_sink_chain));
	gst_element_add_pad (GST_ELEMENT (acm), sink);
	acm->sinkpad = sink;

	src = gst_pad_new_from_static_template (&acmdec_src_template, "src");
	gst_pad_use_fixed_caps (src);
	gst_pad_set_event_function (src, FN(acmdec_src_event));
	gst_pad_set_query_type_function (src, FN(acmdec_src_query_types));
	gst_pad_set_query_function (src, FN(acmdec_src_query));
	gst_element_add_pad (GST_ELEMENT (acm), src);

	acm->srcpad = src;
}

/*
 * Class initialization
 */

static void acmdec_class_init(AcmDecClass *acm_class)
{
	GstElementClass *elem_class = GST_ELEMENT_CLASS(acm_class);
	GObjectClass *gobj_class = G_OBJECT_CLASS(acm_class);
	
	elem_class->change_state = FN(acmdec_change_state);
	gobj_class->dispose = FN(acmdec_dispose);
}

static void acmdec_base_init(gpointer klass)
{
       	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

	gst_element_class_add_pad_template(element_class,
			gst_static_pad_template_get(&acmdec_src_template));
	gst_element_class_add_pad_template(element_class,
			gst_static_pad_template_get(&acmdec_sink_template));

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
	if (!buf || memcmp(buf, acm_id, 3))
		return;

	gst_type_find_suggest(find, GST_TYPE_FIND_MAXIMUM,
			      gst_caps_new_simple ("audio/x-acm", NULL));
}

