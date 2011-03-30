/*
 * libacm plugin for XMMS2
 *
 * Copyright (C) 2011  Marko Kreen
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

#include "xmms/xmms_xformplugin.h"
#include "xmms/xmms_log.h"
#include "xmms/xmms_medialib.h"

#include <glib.h>
#include <string.h>

#include "libacm.h"

struct XmmsACM {
	ACMStream *acm;
	xmms_error_t *error;
};

/*
 * callbacks for libacm
 */

static int acmio_filesize(void *ctx)
{
	xmms_xform_t *xform = ctx;
	int fsize = 0;

	if (!xmms_xform_metadata_get_int (xform,
					  XMMS_MEDIALIB_ENTRY_PROPERTY_SIZE,
					  &fsize))
		fsize = -1;
	return fsize;
}

static int acmio_read(void *ptr, int size, int n, void *ctx)
{
	xmms_xform_t *xform = ctx;
	struct XmmsACM *priv;
	int res;

	g_return_val_if_fail (xform, -1);
	
	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	res = xmms_xform_read (xform, ptr, n*size, priv->error);
	return res;
}

static int acmio_seek(void *ctx, int offset, int whence)
{
	xmms_xform_t *xform = ctx;
	struct XmmsACM *priv;
	int res;
	xmms_xform_seek_mode_t xwhence;

	g_return_val_if_fail (xform, -1);

	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	switch (whence) {
	case SEEK_SET: xwhence = XMMS_XFORM_SEEK_SET; break;
	case SEEK_CUR: xwhence = XMMS_XFORM_SEEK_CUR; break;
	case SEEK_END: xwhence = XMMS_XFORM_SEEK_END; break;
	default:
		return -1;
	}

	res = xmms_xform_seek (xform, offset, xwhence, priv->error);
	return res;
}

static const acm_io_callbacks acmio = {
	.read_func = acmio_read,
	.seek_func = acmio_seek,
	.close_func = NULL,
	.get_length_func = acmio_filesize,
};

/*
 * callbacks for XMMS2
 */

static gboolean
xmms_acm_init (xmms_xform_t *xform)
{
	int err;
	struct XmmsACM *priv;
	xmms_error_t errbuf;

	g_return_val_if_fail (xform, FALSE);

	priv = g_new0 (struct XmmsACM, 1);
	g_return_val_if_fail (priv, FALSE);
	xmms_xform_private_data_set (xform, priv);

	/* open acm */
	memset (&errbuf, 0, sizeof(errbuf));
	priv->error = &errbuf;
	err = acm_open_decoder (&priv->acm, xform, acmio, 0);
	if (err != 0) {
		xmms_log_error("acm_open_decoder failed");
		return FALSE;
	}

	/* set metainfo */
	xmms_xform_metadata_set_int (xform,
				     XMMS_MEDIALIB_ENTRY_PROPERTY_DURATION,
				     acm_time_total (priv->acm));

	xmms_xform_metadata_set_int (xform,
				     XMMS_MEDIALIB_ENTRY_PROPERTY_BITRATE,
				     acm_bitrate (priv->acm));

	/* set output format */
	xmms_xform_outdata_type_add (xform,
				     XMMS_STREAM_TYPE_MIMETYPE,
				     "audio/pcm",
				     XMMS_STREAM_TYPE_FMT_FORMAT,
				     XMMS_SAMPLE_FORMAT_S16,
				     XMMS_STREAM_TYPE_FMT_CHANNELS,
				     acm_channels (priv->acm),
				     XMMS_STREAM_TYPE_FMT_SAMPLERATE,
				     acm_rate (priv->acm),
				     XMMS_STREAM_TYPE_END);

	return TRUE;
}

static void
xmms_acm_destroy (xmms_xform_t *xform)
{
	struct XmmsACM *priv;
	if (xform) {
		priv = xmms_xform_private_data_get (xform);
		if (priv) {
			acm_close (priv->acm);
			g_free (priv);
		}
	}
}

static gint
xmms_acm_read (xmms_xform_t *xform,
	       xmms_sample_t *buf,
	       gint len,
	       xmms_error_t *error)
{
	struct XmmsACM *priv;

	g_return_val_if_fail (xform, -1);

	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	priv->error = error;
	return acm_read_loop (priv->acm, buf, len, 0, 2, 1);
}

static gint64
xmms_acm_seek (xmms_xform_t *xform,
	       gint64 samples,
	       xmms_xform_seek_mode_t whence,
	       xmms_error_t *error)
{
	struct XmmsACM *priv;
	gint64 pos;

	g_return_val_if_fail (xform, -1);

	priv = xmms_xform_private_data_get (xform);
	g_return_val_if_fail (priv, -1);

	/* calc new pos */
	switch (whence) {
	case XMMS_XFORM_SEEK_CUR:
		pos = acm_pcm_tell (priv->acm) + samples;
		break;
	case XMMS_XFORM_SEEK_END:
		pos = acm_pcm_total (priv->acm) + samples;
		break;
	case XMMS_XFORM_SEEK_SET:
		pos = samples;
		break;
	default:
		return -1;
	}

	/* sanitize */
	if (pos < 0)
		pos = 0;
	else if (pos > acm_pcm_total (priv->acm))
		pos = acm_pcm_total (priv->acm);

	/* actual seek */
	priv->error = error;
	return acm_seek_pcm (priv->acm, pos);
}

static gboolean
xmms_acm_plugin_setup (xmms_xform_plugin_t *plugin)
{
	xmms_xform_methods_t methods;

	XMMS_XFORM_METHODS_INIT (methods);
	methods.init = xmms_acm_init;
	methods.destroy = xmms_acm_destroy;
	methods.read = xmms_acm_read;
	methods.seek = xmms_acm_seek;

	xmms_xform_plugin_methods_set (plugin, &methods);

	xmms_xform_plugin_indata_add (plugin,
				      XMMS_STREAM_TYPE_MIMETYPE,
				      "audio/x-acm",
				      NULL);

	xmms_magic_add ("acm header", "audio/x-acm",
			"0 lelong 0x01032897",
			NULL);

	xmms_magic_add ("wavc header", "audio/x-acm",
			"0 string WAVC",
			NULL);

	xmms_magic_extension_add ("audio/x-acm", "*.acm");

	return TRUE;
}

/*
 * plugin info
 */

XMMS_XFORM_PLUGIN ("acm",
		   "Interplay ACM Decoder",
		   LIBACM_VERSION,
		   "libacm plugin for XMMS2",
		   xmms_acm_plugin_setup);

