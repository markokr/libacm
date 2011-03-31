/*
 * libacm plugin for Audacious
 *
 * Copyright (C) 2004-2011  Marko Kreen
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

#define GDK_DISABLE_DEPRECATED
#define GTK_DISABLE_DEPRECATED

#include <audacious/plugin.h>

#include "libacm.h"


static int acmx_seek_to = -1;

static int acmx_open_vfs(ACMStream **acm_p, const gchar *url);

/*
 * useful stuff
 */

static gchar *get_title(const gchar * filename)
{
	const gchar *p = strrchr(filename, '/');
	p = (p != NULL) ? (p + 1) : filename;
	return g_strdup(p);
}


/*
 * module functions
 */

static gint acmx_is_our_file(const gchar * filename)
{
	ACMStream *acm;

	if (acmx_open_vfs(&acm, filename) < 0)
		return FALSE;
	acm_close(acm);
	return TRUE;
}

static Tuple *acmx_get_song_tuple(const gchar * filename)
{
	ACMStream *acm;
	const ACMInfo *info;
	int err;
	Tuple *tup = NULL;
	char buf[512];
	gchar *title, *ext;

	ext = strrchr(filename, '.');
	if (!ext || strcasecmp(ext, ".acm") != 0)
		return NULL;

	if ((err = acmx_open_vfs(&acm, filename)) < 0)
		return NULL;

	tup = tuple_new_from_filename(filename);

	title = get_title(filename);
	tuple_associate_string(tup, FIELD_TITLE, NULL, title);
	g_free(title);

	info = acm_info(acm);
	snprintf(buf, sizeof(buf), "acm-level=%d acm-subblocks=%d",
		 info->acm_level, info->acm_rows);
	tuple_associate_string(tup, FIELD_COMMENT, NULL, buf);

	tuple_associate_int(tup, FIELD_LENGTH, NULL, acm_time_total(acm));
	tuple_associate_int(tup, FIELD_BITRATE, NULL, acm_bitrate(acm) / 1024);
	tuple_associate_string(tup, FIELD_CODEC, NULL, "InterPlay ACM");
	tuple_associate_string(tup, FIELD_MIMETYPE, NULL, "application/acm");
	tuple_associate_string(tup, FIELD_QUALITY, NULL, "lossy");
	
	acm_close(acm);
	return tup;
}

static void acmx_pause(InputPlayback *pback, gshort p)
{
	pback->output->pause(p);
}

static void acmx_seek(InputPlayback *pback, gint secs)
{
	acmx_seek_to = secs * 1000;
	while (acmx_seek_to >= 0)
		g_usleep(20000);
}


static void read_and_play(ACMStream *acm, InputPlayback *pback, gchar *buf, int block_len)
{
	int got_bytes;

	got_bytes = acm_read_loop(acm, buf, block_len, 0, 2, 1);
	if (got_bytes > 0) {
		pback->pass_audio(pback, FMT_S16_LE, acm_channels(acm), got_bytes, buf, &pback->playing);
	} else {
		/* flush buffer */
		while (pback->output->buffer_playing()) {
			if (!pback->playing)
				break;
			g_usleep(10000);
		}
		pback->eof = TRUE;
	}
}

static void try_seeking(ACMStream *acm, InputPlayback *pback)
{
	int pos = acm_seek_time(acm, acmx_seek_to);
	
	if (pos >= 0) {
		pback->output->flush(acm_time_tell(acm));
		pback->eof = 0;
	}
	acmx_seek_to = -1;
}

#define BLK_SAMPLES 512

static void play_file(InputPlayback *pback)
{
	gchar *filename = pback->filename;
	gint res;
	ACMStream *acm;
	int err, block_len;
	gchar *buf;
	
	if ((err = acmx_open_vfs(&acm, filename)) < 0)
		return;

	pback->set_params(pback, NULL, 0, acm_bitrate(acm), acm_rate(acm), acm_channels(acm));

	res = pback->output->open_audio(FMT_S16_LE, acm_rate(acm), acm_channels(acm));
	if (res == 0) {
		pback->error = TRUE;
		acm_close(acm);
		return;
	}

	/*
	 * main loop
	 */
	block_len = BLK_SAMPLES * acm_channels(acm) * ACM_WORD;
	buf = g_malloc0(block_len);
	while (pback->playing) {
		if (acmx_seek_to >= 0)
			try_seeking(acm, pback);
		
		if (!pback->eof) {
			read_and_play(acm, pback, buf, block_len);
		} else if (pback->output->buffer_playing()) {
			g_usleep(10000);
		} else {
			break;
		}
	}

	if (!pback->error) {
		/* flush buffer */
		while (pback->eof && pback->output->buffer_playing()) {
			g_usleep(10000);
		}
	}

	g_free(buf);
	acm_close(acm);
	pback->output->close_audio();
	pback->playing = 0;
}

static void acmx_play_file(InputPlayback *pback)
{
	pback->playing = 1;
	pback->eof = 0;
	pback->error = FALSE;

	pback->set_pb_ready(pback);

	play_file(pback);
}

static void acmx_stop(InputPlayback *pback)
{
	pback->playing = 0;
}

/*
 * Plugin info.
 */

static const gchar * const acmx_fmts[] = { "acm", NULL };

static InputPlugin acmx_plugin = {
	.description = "InterPlay ACM Audio Plugin",

	.is_our_file = acmx_is_our_file,
	.play_file = acmx_play_file,
	.stop = acmx_stop,
	.pause = acmx_pause,
	.seek = acmx_seek,

	.get_song_tuple = acmx_get_song_tuple,	/* aud 1.1.0 */
	.vfs_extensions = acmx_fmts,		/* aud 1.3.0 */
};

static InputPlugin *acmx_plugin_list[] = { &acmx_plugin, NULL };
SIMPLE_INPUT_PLUGIN(libacm, acmx_plugin_list);


/*
 * vfs i/o funcs.
 */

static int acmx_vfs_read(void *dst, int size, int n, void *arg)
{
	VFSFile *f = arg;
	return vfs_fread(dst, size, n, f);
}

static int acmx_vfs_seek(void *arg, int offset, int whence)
{
	VFSFile *f = arg;
	return vfs_fseek(f, offset, whence);
}

static int acmx_vfs_close(void *arg)
{
	VFSFile *f = arg;
	vfs_fclose(f);
	return 0;
}

static int acmx_vfs_get_length(void *arg)
{
	VFSFile *f = arg;
	return vfs_fsize(f);
}

static const acm_io_callbacks acmx_vfs_cb = {
	acmx_vfs_read,
	acmx_vfs_seek,
	acmx_vfs_close,
	acmx_vfs_get_length
};

static int acmx_open_vfs(ACMStream **acm_p, const gchar *url)
{
	VFSFile *stream;
	int res;

	stream = vfs_fopen(url, "r");
	if (stream == NULL)
		return ACM_ERR_OPEN;

	res = acm_open_decoder(acm_p, stream, acmx_vfs_cb, 0);
	if (res < 0)
		vfs_fclose(stream);
	return res;
}

