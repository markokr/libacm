/*
 * libacm plugin for Audacious
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

#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <pthread.h>
#include <glib.h>
#include <gtk/gtk.h>

#include <audacious/plugin.h>
#include <audacious/util.h>

#include "libacm.h"

#define acm_debug(fmt, args...)  do { \
	if (0) { \
	char buf[256]; snprintf(buf, sizeof(buf), fmt, ## args); \
	printf("%s(%d): %s\n", __func__, __LINE__, buf); \
} } while (0)

static GThread *decode_thread;
static int acmx_seek_to = -1;

static int acmx_open_vfs(ACMStream **acm_p, gchar *url);

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

static gint acmx_is_our_file(gchar * filename)
{
	ACMStream *acm;

	if (acmx_open_vfs(&acm, filename) < 0)
		return FALSE;
	acm_close(acm);
	return TRUE;
}

static Tuple *acmx_get_song_tuple(gchar * filename)
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

	tup = aud_tuple_new_from_filename(filename);

	title = get_title(filename);
	aud_tuple_associate_string(tup, FIELD_TITLE, NULL, title);
	g_free(title);

	info = acm_info(acm);
	snprintf(buf, sizeof(buf), "acm-level=%d acm-subblocks=%d",
		 info->acm_level, info->acm_rows);
	aud_tuple_associate_string(tup, FIELD_COMMENT, NULL, buf);

	aud_tuple_associate_int(tup, FIELD_LENGTH, NULL, acm_time_total(acm));
	aud_tuple_associate_int(tup, FIELD_BITRATE, NULL, acm_bitrate(acm) / 1024);
	aud_tuple_associate_string(tup, FIELD_CODEC, NULL, "InterPlay ACM");
	aud_tuple_associate_string(tup, FIELD_MIMETYPE, NULL, "application/acm");
	aud_tuple_associate_string(tup, FIELD_QUALITY, NULL, "lossy");
	
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

#define BLK_SAMPLES 512

static void read_and_play(ACMStream *acm, InputPlayback *pback, gchar *buf)
{
	int got_bytes, need_bytes;

	need_bytes = BLK_SAMPLES * acm_channels(acm) * ACM_WORD;
	got_bytes = acm_read_loop(acm, buf, need_bytes, 0, 2, 1);
	if (got_bytes > 0) {
		pback->plugin->add_vis_pcm(pback->output->written_time(),
			FMT_S16_LE, acm_channels(acm), got_bytes, buf);

		while (pback->output->buffer_free() < got_bytes
				&& pback->playing && acmx_seek_to == -1)
			g_usleep(10000);

		if (pback->playing && acmx_seek_to == -1)
			pback->output->write_audio(buf, got_bytes);
	} else {
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

static void play_file(InputPlayback *pback)
{
	gchar *filename = pback->filename;
	gchar *name;
	gint res;
	ACMStream *acm;
	int err;
	gchar *buf;
	
	if ((err = acmx_open_vfs(&acm, filename)) < 0)
		return;

        name = get_title(filename);
        pback->set_params(pback, name, acm_time_total(acm), acm_bitrate(acm),
			  acm_rate(acm), acm_channels(acm));
	g_free(name);

	res = pback->output->open_audio(FMT_S16_LE, acm_rate(acm), acm_channels(acm));
	if (res == 0) {
		pback->error = TRUE;
		acm_close(acm);
		return;
        }

	/*
	 * main loop
	 */
	buf = g_malloc0(BLK_SAMPLES * ACM_WORD * acm_channels(acm));
	while (pback->playing) {
		if (acmx_seek_to >= 0)
			try_seeking(acm, pback);
		
		if (!pback->eof) {
			read_and_play(acm, pback, buf);
		} else {
			pback->playing = 0;
		}
	}

	if (!pback->error) {
		/* flush buffer */
		while (pback->eof && pback->output->buffer_playing()) {
			g_usleep(50000);
		}

		pback->output->close_audio();
	}

	g_free(buf);
	acm_close(acm);
}

static void acmx_play_file(InputPlayback *pback)
{
	pback->playing = 1;
	pback->eof = 0;
	pback->error = FALSE;

	decode_thread = g_thread_self();
	pback->set_pb_ready(pback);

	play_file(pback);
}

static void acmx_stop(InputPlayback *pback)
{
	if (pback->playing) {
		pback->playing = 0;
		g_thread_join(decode_thread);
		decode_thread = NULL;
	}
}

static void acmx_about(void)
{
	static GtkWidget *about_window = NULL;
	const char *msg;

	if (about_window) {
		gtk_window_present(GTK_WINDOW(about_window));
		return;
	}

	msg = "InterPlay ACM Audio Plugin for Audacious\n\n"
		"libacm " LIBACM_VERSION "\n\n"
		"Homepage: http://libacm.berlios.de/\n\n";
	about_window = audacious_info_dialog("About", msg, "Ok", FALSE, NULL, NULL);
	g_signal_connect(G_OBJECT(about_window), "destroy",
			G_CALLBACK(gtk_widget_destroyed), &about_window);
}

/*
 * Plugin info.
 */

#ifdef HAVE_AUDACIOUS_1_3
static gchar *acmx_fmts[] = { "acm", NULL };
#endif

static InputPlugin acmx_plugin = {
	.description = "InterPlay ACM Audio Plugin",

	.about = acmx_about,
	.is_our_file = acmx_is_our_file,
	.play_file = acmx_play_file,
	.stop = acmx_stop,
	.pause = acmx_pause,
	.seek = acmx_seek,

	.get_song_tuple = acmx_get_song_tuple,	/* aud 1.1.0 */
#ifdef HAVE_AUDACIOUS_1_3
	.vfs_extensions = acmx_fmts,		/* aud 1.3.0 */
#endif
};

static InputPlugin *acmx_plugin_list[] = { &acmx_plugin, NULL };
SIMPLE_INPUT_PLUGIN(libacm, acmx_plugin_list);


/*
 * vfs i/o funcs.
 */

static int acmx_vfs_read(void *dst, int size, int n, void *arg)
{
	VFSFile *f = arg;
	return aud_vfs_fread(dst, size, n, f);
}

static int acmx_vfs_seek(void *arg, int offset, int whence)
{
	VFSFile *f = arg;
	return aud_vfs_fseek(f, offset, whence);
}

static int acmx_vfs_close(void *arg)
{
	VFSFile *f = arg;
	aud_vfs_fclose(f);
	return 0;
}

static int acmx_vfs_get_length(void *arg)
{
	VFSFile *f = arg;
	return aud_vfs_fsize(f);
}

static const acm_io_callbacks acmx_vfs_cb = {
	acmx_vfs_read,
	acmx_vfs_seek,
	acmx_vfs_close,
	acmx_vfs_get_length
};

static int acmx_open_vfs(ACMStream **acm_p, gchar *url)
{
	VFSFile *stream;
	int res;

	stream = aud_vfs_fopen(url, "r");
	if (stream == NULL)
		return ACM_ERR_OPEN;

	res = acm_open_decoder(acm_p, stream, acmx_vfs_cb);
	if (res < 0)
		aud_vfs_fclose(stream);
	return res;
}

