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
#include <sys/time.h>
#include <time.h>
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

typedef unsigned long long usec_t;
#define USEC 1000000

#define ACMDEC_SEEK_WAIT (USEC / 5)

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

	int fileofs;
	gboolean discont;

	int seek_to_pcm;
	usec_t last_seek_event_time;
} AcmDec;

typedef struct AcmDecClass {
	GstElementClass parent_class;
} AcmDecClass;

/*
 * define element class
 */

#define TYPE_ACMDEC		(acmdec_get_type())
#define ACMDEC(o)		G_TYPE_CHECK_INSTANCE_CAST((o),TYPE_ACMDEC,AcmDec)
#define ACMDEC_CLASS(k)		G_TYPE_CHECK_CLASS_CAST((k),TYPE_ACMDEC,AcmDecClass)
#define IS_ACMDEC(o)		G_TYPE_CHECK_INSTANCE_TYPE((o),TYPE_ACMDEC)
#define IS_ACMDEC_CLASS(c)	G_TYPE_CHECK_CLASS_TYPE((c),TYPE_ACMDEC)

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
			"rate = (int) [ 4000, 48000 ], "
			"channels = (int) [ 1, 2 ]")
);

static GstStaticPadTemplate acmdec_sink_template =
GST_STATIC_PAD_TEMPLATE (
	"sink", GST_PAD_SINK, GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/x-acm"));


/*
 * File type handler.
 */

static void acmdec_detect_file(GstTypeFind *find, gpointer junk)
{
	guint8 *buf;
	static const guint8 acm_id[] = { 0x97, 0x28, 0x03 };

	buf = gst_type_find_peek(find, 0, 3);
	if (!buf || memcmp(buf, acm_id, 3))
		return;

	gst_type_find_suggest(find, GST_TYPE_FIND_MAXIMUM,
			      gst_caps_new_simple ("audio/x-acm", NULL));
}

/*
 * Fire reader with gst_pad_pull_range()
 */

static int acmdec_pull_read(void *dst, int size, int n, void *arg)
{
	unsigned int got, need_bytes = size * n;
	AcmDec *acm = arg;
	GstBuffer *buf = NULL;
	GstFlowReturn flow;
	void *data;

	flow = gst_pad_pull_range(acm->sinkpad, acm->fileofs, need_bytes, &buf);
	if (flow != GST_FLOW_OK)
		return -1;

	data = GST_BUFFER_DATA(buf);
	got = GST_BUFFER_SIZE(buf);

	memcpy(dst, data, got);
	acm->fileofs += got;

	return got;
}

static int acmdec_pull_seek(void *arg, int ofs, int whence)
{
	AcmDec *acm = arg;
	switch (whence) {
	case SEEK_SET:
		acm->fileofs = ofs;
		break;
	case SEEK_CUR:
		acm->fileofs += ofs;
		break;
	case SEEK_END:
		/* unsupported */
		return -1;
	}
	return acm->fileofs;
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

static usec_t get_real_time(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return USEC * (usec_t)tv.tv_sec + tv.tv_usec;
}

/*
 * Info sending.
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

static void acmdec_send_segment(AcmDec *acm, gint64 pcmpos)
{
	GstEvent *ev;
	guint64 start, stop, curpos;

	curpos = GST_SECOND * pcmpos / acm_rate(acm->ctx);
	stop = GST_SECOND * acm_time_total(acm->ctx) / 1000;
	start = curpos;

	GST_DEBUG_OBJECT(acm, "sending segment");

	ev = gst_event_new_new_segment(FALSE, 1.0, GST_FORMAT_TIME, start, stop, curpos);
	if (!gst_pad_push_event(acm->srcpad, ev))
		GST_DEBUG_OBJECT(acm, "sending segment failed");
	else
		GST_DEBUG_OBJECT(acm, "sending segment done");
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


	if (!acm->ctx) {
		GST_ERROR_OBJECT(acm, "no stream open");
		return FALSE;
	}

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
		goto no_format;
	}

done:
	return res;

no_format:
	GST_ERROR_OBJECT(acm, "formats unsupported");
	res = FALSE;
	goto done;
}

/*
 * Object init
 */

static gboolean acmdec_init_decoder(AcmDec *acm)
{
	static const acm_io_callbacks pull_cb = {
		.read_func = acmdec_pull_read,
		.seek_func = acmdec_pull_seek,
		.get_length_func = acmdec_io_get_size,
	};

	int res;
	GstCaps *caps;

	GST_DEBUG_OBJECT(acm, "init decoder");
	res = acm_open_decoder(&acm->ctx, acm, pull_cb, 0);
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

	acm->fileofs = 0;
	acm->discont = TRUE;
	acm->seek_to_pcm = -1;
	acm->last_seek_event_time = 0;
}

static void acmdec_dispose(GObject *obj)
{
	AcmDec *acm = ACMDEC(obj);

	acmdec_reset(acm);

	G_OBJECT_CLASS (parent_class)->dispose (obj);
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
		if (acm->seek_to_pcm >= 0)
			pcmval = acm->seek_to_pcm;
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

static gboolean handle_seek(AcmDec *acm, GstEvent *event)
{
	GstFormat format, tformat;
	gdouble rate;
	GstSeekFlags flags;
	GstSeekType cur_type, stop_type;
	gint64 cur, stop, pcmpos;

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
	if (!acmdec_convert (acm, format, cur, &tformat, &pcmpos))
		return FALSE;

	if (pcmpos != acm_rate(acm->ctx) * cur / GST_SECOND) {
		GST_ERROR_OBJECT(acm, "do_seek: bad conversion");
	}

	GST_DEBUG_OBJECT(acm, "do_seek: newpos=%llu curpos=%d",
			 pcmpos, (int)acm_pcm_tell(acm->ctx));

	/*
	 * Set seek pos.  Lock as touched from play thread too.
	 */

	GST_OBJECT_LOCK(acm);
	acm->seek_to_pcm = pcmpos;
	acm->last_seek_event_time = get_real_time();
	GST_OBJECT_UNLOCK(acm);

	return TRUE;
}

static gboolean acmdec_src_event(GstPad *pad, GstEvent *event)
{
	AcmDec *acm = ACMDEC(gst_pad_get_parent(pad)); /* incref */
	gboolean res = FALSE;


	switch (GST_EVENT_TYPE (event)) {
	case GST_EVENT_SEEK:
		res = handle_seek(acm, event);
		gst_event_unref(event);
		break;
	default:
		GST_DEBUG_OBJECT (acm, "src received %s event", GST_EVENT_TYPE_NAME (event));
	case GST_EVENT_QOS:
	case GST_EVENT_NAVIGATION:
		res = gst_pad_event_default (pad, event);
		break;
	}
	gst_object_unref(acm);
	return res;
}

/*
 * new block of data.
 */

static gboolean acmdec_src_check_get_range (GstPad *pad)
{
	GST_DEBUG_OBJECT(GST_PAD_PARENT(pad), "check_get_range");
	return TRUE;
}

// GST_FLOW_UNEXPECTED -> EOS
static GstFlowReturn acmdec_src_get_range(GstPad *srcpad, guint64 offset, guint size, GstBuffer **buf)
{
	AcmDec *acm = ACMDEC(GST_PAD_PARENT(srcpad));
	unsigned int req_pos;
	int got, frame;
	gint64 pcmpos, pcmlen;
	GstFlowReturn flow = GST_FLOW_ERROR;

	*buf = NULL;

	//GST_DEBUG_OBJECT(acm, "get_range");

	if (!acm->ctx) {
		if (!acmdec_init_decoder(acm)) {
			GST_ERROR_OBJECT(acm, "cannot initialize stream");
			goto error;
		}
	}

	frame = ACM_WORD * acm_channels(acm->ctx);
	if (offset % frame || size % frame) {
		GST_ERROR_OBJECT(acm, "request not multiple of frame (%d)", frame);
		goto error;
	}

	req_pos = offset / frame;
	if (acm_pcm_tell(acm->ctx) != req_pos) {
		GST_INFO_OBJECT(acm, "seeking: cur=%d, new=%d", acm_pcm_tell(acm->ctx), req_pos);
		if (acm_seek_pcm(acm->ctx, req_pos) < 0) {
			GST_ERROR_OBJECT(acm, "seek failed");
			goto error;
		}
		if (acm_pcm_tell(acm->ctx) != req_pos) {
			GST_ERROR_OBJECT(acm, "seek failed to reach right pos");
			goto error;
		}
	}

	flow = gst_pad_alloc_buffer_and_set_caps(srcpad, GST_CLOCK_TIME_NONE,
						 size, GST_PAD_CAPS(srcpad), buf);
	if (flow != GST_FLOW_OK)
		goto error;

	pcmpos = acm_pcm_tell(acm->ctx);
	got = acm_read_loop(acm->ctx, GST_BUFFER_DATA(*buf), size, ACM_NATIVE_BE, 2, 1);
	if (got < 0) {
		flow = GST_FLOW_ERROR;
		goto error;
	} else if (got == 0) {
		/* EOS */
		flow = GST_FLOW_UNEXPECTED;
		goto error;
	}
	pcmlen = got / frame;
	GST_BUFFER_SIZE (*buf) = got;
	GST_BUFFER_TIMESTAMP (*buf) = GST_SECOND * pcmpos / acm_rate(acm->ctx);
	GST_BUFFER_DURATION (*buf) = GST_SECOND * pcmlen / acm_rate(acm->ctx);

	return GST_FLOW_OK;

error:
	if (*buf) {
		gst_buffer_unref(*buf);
		*buf = NULL;
	}
	return flow;
}

/*
 * Sinkpad methods.  If downstream is push-based,
 * launch separate thread for pull->push handling.
 */

static void do_real_seek(AcmDec *acm)
{
	int seek_to_pcm;
	usec_t seek_time;

	GST_DEBUG_OBJECT(acm, "do_real_seek");

	GST_OBJECT_LOCK(acm);
	seek_to_pcm = acm->seek_to_pcm;
	seek_time = acm->last_seek_event_time;
	GST_OBJECT_UNLOCK(acm);

	if (seek_to_pcm < 0)
		return;
	if (seek_time + ACMDEC_SEEK_WAIT > get_real_time())
		return;

	gst_pad_push_event (acm->srcpad, gst_event_new_flush_start ());

	if (acm_seek_pcm(acm->ctx, seek_to_pcm) < 0) {
		GST_ERROR_OBJECT(acm, "acm_seek_pcm failed");
	}

	GST_DEBUG_OBJECT(acm, "reached seek pos at %d", (int)acm_pcm_tell(acm->ctx));
	GST_OBJECT_LOCK(acm);
	acm->seek_to_pcm = -1;
	acm->discont = TRUE;
	GST_OBJECT_UNLOCK(acm);

	gst_pad_push_event(acm->srcpad, gst_event_new_flush_stop());
}

static void acmdec_sink_loop(void *arg)
{
	GstPad *sinkpad = (GstPad *)arg;
	AcmDec *acm = ACMDEC(GST_PAD_PARENT(sinkpad));
	GstFlowReturn flow;
	gint64 size, offset, pcmpos;
	GstBuffer *buf = NULL;
	int frame;

	if (!acm->ctx) {
		if (!acmdec_init_decoder(acm)) {
			GST_ERROR_OBJECT(acm, "cannot initialize stream");
			goto pause;
		}
	}

	if (acm->seek_to_pcm >= 0)
		do_real_seek(acm);

	frame = ACM_WORD * acm_channels(acm->ctx);
	pcmpos = acm_pcm_tell(acm->ctx);
	offset = pcmpos * frame;
	size = acm->ctx->block_len * frame;

	flow = acmdec_src_get_range(acm->srcpad, offset, size, &buf);
	if (flow == GST_FLOW_UNEXPECTED)
		goto eos_and_pause;
	else if (flow != GST_FLOW_OK) {
		GST_DEBUG_OBJECT (acm, "get_range failed: %s", gst_flow_get_name (flow));
		goto pause;
	}

	if (acm->discont) {
		acmdec_send_segment(acm, pcmpos);

		buf = gst_buffer_make_metadata_writable(buf);
		GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DISCONT);
		acm->discont = FALSE;
	}

	flow = gst_pad_push(acm->srcpad, buf);
	if (flow != GST_FLOW_OK) {
		GST_DEBUG_OBJECT(acm, "gst_pad_push failed: %s", gst_flow_get_name(flow));
		goto pause;
	}
	return;

eos_and_pause:
	GST_DEBUG_OBJECT(acm, "EOS");
	gst_pad_push_event(acm->srcpad, gst_event_new_eos());
pause:
	GST_DEBUG_OBJECT(acm, "pausing task");
	gst_pad_pause_task(acm->sinkpad);
}


static gboolean acmdec_sink_activate_pull (GstPad *sinkpad, gboolean active)
{
	GST_DEBUG_OBJECT(GST_PAD_PARENT(sinkpad), "activate_pull: %d", active);
	if (active) {
		return gst_pad_start_task(sinkpad, acmdec_sink_loop, sinkpad);
	} else {
		return gst_pad_stop_task(sinkpad);
	}
}

static gboolean acmdec_sink_activate (GstPad *sinkpad)
{
	GST_DEBUG_OBJECT(GST_PAD_PARENT(sinkpad), "sink_activate");
	if (gst_pad_check_pull_range(sinkpad))
		return gst_pad_activate_pull(sinkpad, TRUE);
	return FALSE;
}


static gboolean acmdec_sink_activate_push(GstPad *pad, gboolean active)
{
	GST_DEBUG_OBJECT(GST_PAD_PARENT(pad), "push active = %d", active);
	return FALSE;
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

	ret = parent_class->change_state(elem, transition);
	if (ret != GST_STATE_CHANGE_SUCCESS) {
		GST_DEBUG_OBJECT(acm, "parent change_state: %s",
				 gst_element_state_change_return_get_name(ret));
		return ret;
	}

	if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
		acmdec_reset(acm);

	return ret;
}

/*
 * GST initalization.
 */

static void acmdec_init(AcmDec *acm, AcmDecClass *klass)
{
	GstPad *src, *sink;

	GST_DEBUG_OBJECT(acm, "acmdec_init");

	acmdec_reset(acm);

	sink = gst_pad_new_from_static_template (&acmdec_sink_template, "sink");
	gst_pad_set_activate_function (sink, FN(acmdec_sink_activate));
	gst_pad_set_activatepull_function (sink, FN(acmdec_sink_activate_pull));
	gst_pad_set_activatepush_function (sink, FN(acmdec_sink_activate_push));
	gst_element_add_pad (GST_ELEMENT (acm), sink);
	acm->sinkpad = sink;

	src = gst_pad_new_from_static_template (&acmdec_src_template, "src");
	gst_pad_use_fixed_caps (src);
	gst_pad_set_event_function (src, FN(acmdec_src_event));
	gst_pad_set_query_type_function (src, FN(acmdec_src_query_types));
	gst_pad_set_query_function (src, FN(acmdec_src_query));
	/* do those get actually used? */
	gst_pad_set_checkgetrange_function (src, FN(acmdec_src_check_get_range));
	gst_pad_set_getrange_function (src, FN(acmdec_src_get_range));
	gst_element_add_pad (GST_ELEMENT (acm), src);

	acm->srcpad = src;
}

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

