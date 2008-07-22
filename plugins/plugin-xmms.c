/*
 * libacm plugin for XMMS
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

#include <plugin.h>
#include <util.h>

#include "libacm.h"

#define FORCE_CHANS 2

typedef struct {
	ACMStream *acm;
	short eof, going;
	int seek_to;
} ACMInput;

static ACMInput *input = NULL;
static InputPlugin *plugin;

static gboolean audio_error = FALSE;
static pthread_t decode_thread;

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
static void init(void)
{
}

static void quit(void)
{
}

static gint is_our_file(gchar * filename)
{
	static const unsigned char _id[] = { 0x97, 0x28, 0x03, 0x01 };
	int res;
	char buf[4];
	FILE *f;
	gchar *ext;
	
	if ((ext = strrchr(filename, '.')) == NULL)
		return FALSE;
	if (strcasecmp(ext, ".acm") != 0)
		return FALSE;
	if ((f = fopen(filename, "rb")) == NULL)
		return FALSE;
	res = fread(buf, 1, 4, f);
	fclose(f);
	if (res != 4 || memcmp(buf, _id, 4) != 0)
		return FALSE;
	return TRUE;
}

static void get_song_info(gchar * filename, gchar ** title, gint * length)
{
	ACMStream *acm;
	int err;

	if ((err = acm_open_file(&acm, filename, FORCE_CHANS)) < 0) {
		fprintf(stderr, "get_song_info: %d\n", err);
		return;
	}

	*length = acm_time_total(acm);
	*title = get_title(filename);
	
	acm_close(acm);
}

static void pause(gshort p)
{
	plugin->output->pause(p);
}

static void seek(gint secs)
{
	input->seek_to = secs * 1000;
	input->eof = FALSE;
}

static int get_time(void)
{
	if (audio_error)
		return -2;
	if (!input || !input->going)
		return -1;
	if (input->seek_to >= 0)
		return input->seek_to;
        if (input->eof && !plugin->output->buffer_playing())
		return -1;
	else {
		return plugin->output->output_time();
	}
}

#define BLK_SAMPLES 512

static void read_and_play(gchar *buf) {
	int got_bytes;
	ACMStream *acm = input->acm;
	const ACMInfo *info = acm_info(acm);

	got_bytes = acm_read_loop(acm, buf,
			BLK_SAMPLES * info->channels * ACM_WORD,
			0, 2, 1);

	if (got_bytes > 0) {
		plugin->add_vis_pcm(plugin->output->written_time(),
			FMT_S16_LE, info->channels, got_bytes, buf);

		while (plugin->output->buffer_free() < got_bytes
				&& input->going && input->seek_to == -1)
			xmms_usleep(10000);

		if (input->going && input->seek_to == -1)
			plugin->output->write_audio(buf, got_bytes);
	} else {
		input->eof = TRUE;
		plugin->output->buffer_free();
		plugin->output->buffer_free();
		xmms_usleep(10000);		
	}
}

static void try_seeking()
{
	int pos = acm_seek_time(input->acm, input->seek_to);
	
	if (pos >= 0) {
		plugin->output->flush(acm_time_tell(input->acm));
		input->eof = 0;
	}
	input->seek_to = -1;
}

static void * play_thread(void * arg)
{
	const ACMInfo *info = acm_info(input->acm);
	gchar *buf = g_malloc0(BLK_SAMPLES * ACM_WORD * info->channels);
	while (input->going) {
		if (input->seek_to != -1)
			try_seeking();
		
		if (!input->eof) {
			read_and_play(buf);
		} else {
			xmms_usleep(10000);
		}

	}
	g_free(buf);
	pthread_exit(NULL);
	return NULL;
}

static void play_file(gchar * filename)
{
	gchar *name;
	gulong ftime;
	gint res;
	ACMStream *acm;
	const ACMInfo *info;
	int err;
	
	audio_error = FALSE;
	
	if (input) {
		if (input->acm)
			acm_close(input->acm);
		g_free(input);
		input = NULL;
	}
	if ((err = acm_open_file(&acm, filename, FORCE_CHANS)) < 0)
		return;

	info = acm_info(acm);
	
	input = g_malloc(sizeof(ACMInput));
	memset(input, 0, sizeof(ACMInput));
	
	input->acm = acm;
        input->going = 1;
        input->seek_to = -1;
	
	res = plugin->output->open_audio(FMT_S16_LE,
			info->rate, info->channels);
	if (res == 0) {
            audio_error = TRUE;
	    acm_close(acm);
	    g_free(input);
	    input = NULL;
            return;
        }
        name = get_title(filename);
	ftime = acm_time_total(acm);
        plugin->set_info(name, ftime,
			acm_bitrate(acm),
                        info->rate,
			info->channels);
	g_free(name);
	pthread_create(&decode_thread, NULL, play_thread, NULL);
}

static void stop(void)
{
    	if (input && input->going) {
	    	input->going = 0;
	     	pthread_join(decode_thread, NULL);
	      	plugin->output->close_audio();
		if (input->acm)
			acm_close(input->acm);
		g_free(input);
		input = NULL;
	}
}

static void file_info_box(char *filename)
{
	char buf[1024];
	ACMStream *acm;
	int err, kbps, secs;
	GtkWidget *dlg;
	const ACMInfo *inf;

	err = acm_open_file(&acm, filename, FORCE_CHANS);
	if (err < 0)
		return;
	
	kbps = acm_bitrate(acm) / 1000;
	secs = acm_time_total(acm) / 1000;

	inf = acm_info(acm);
	
	sprintf(buf, "%s\n\n"
			"Length: %d:%02d\n"
			"Samples: %d\n"
			"Samplerate: %d Hz\n"
			"Channels: %d\n"
			"Avg. bitrate: %d kbps\n\n"
			"ACM subblock len=%d\n"
			"ACM num subblocks=%d\n"
			"ACM block=%d",
			filename, secs/60, secs % 60,
			acm_pcm_total(acm), inf->rate,
			inf->channels, kbps,
			inf->acm_cols, inf->acm_rows,
			inf->acm_cols * inf->acm_rows);
	acm_close(acm);	
	
	dlg = xmms_show_message("File Info", buf, "Ok", FALSE, NULL, NULL);
	gtk_signal_connect(GTK_OBJECT(dlg), "destroy",
			GTK_SIGNAL_FUNC(gtk_widget_destroyed), NULL);
}

static void about()
{
	static GtkWidget *about_window;
	if (about_window) {
		gdk_window_raise(about_window->window);
		return;
	}
	about_window = xmms_show_message("About",
			"InterPlay ACM Audio Decoder - libacm "
			LIBACM_VERSION "\n\n"
			"Homepage: http://libacm.berlios.de/\n"
			"\n",
		"Ok", FALSE, NULL, NULL);
	gtk_signal_connect(GTK_OBJECT(about_window), "destroy",
			GTK_SIGNAL_FUNC(gtk_widget_destroyed), NULL);
}


static InputPlugin _plugin = {
	NULL,			/* handle */
	NULL,			/* filename */
	"InterPlay ACM Audio Plugin (libacm " LIBACM_VERSION ")",
	init,
	about,
	NULL,			/* configure */
	is_our_file,
	NULL,			/* scan_dir */
	play_file,
	stop,
	pause,
	seek,
	NULL,			/* set_eq */
	get_time,
	NULL,			/* get_volume */
	NULL,			/* set_volume */
	quit,			/* cleanup */
	NULL,			
	NULL,
	NULL,
	NULL,
	get_song_info,
	file_info_box,
	NULL			/* output */
};

InputPlugin *get_iplugin_info(void)
{
	plugin = &_plugin;
	return &_plugin;
}

