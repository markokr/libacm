/*
 * libacm plugin for Winamp
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

#include <windows.h>
#include <stdio.h>

#include "winamp.h"
#include "libacm.h"

/* 576 samples seems preferred by winamp */
#define SBLOCK 576

/* eof msg to winamp */
#define WM_AMP_EOF (WM_USER + 2)

typedef struct {
	ACMStream *acm;
	int paused;
	int seek_to;
	char *filename;
} ACMInput;

static ACMInput in;
static In_Module *plugin;

static int dec_quit = 0;
static HANDLE dec_thread = INVALID_HANDLE_VALUE;

/*
 * module functions
 */
static void init()
{
}

static void quit()
{
}

static void config(HWND hwndParent)
{
	MessageBox(hwndParent, "No configuration.",
		"InterPlay ACM Audio Decoder", MB_OK);
}

static int is_our_file(char *fn)
{
	/* used for detecting URL streams.. unused here. */
	return 0;
}

static void get_song_info(char *filename, char *title, int *length_in_ms)
{
	char *fn, *p;
	ACMStream *acm;

	if (filename && *filename) {
		if (acm_open_file(&acm, filename) < 0)
			return;
		fn = filename;
	} else {
		acm = in.acm;
		fn = in.filename;
	}
	if ((p = strrchr(fn, '\\')) != NULL)
		strcpy(title, p + 1);
	else
		strcpy(title, fn);
	
	*length_in_ms = acm_time_total(acm);

	if (filename && *filename)
		acm_close(acm);
}

static void pause()
{
	in.paused = 1;
	plugin->outMod->Pause(1);
}

static void unpause()
{
	in.paused = 0;
	plugin->outMod->Pause(0);
}

static int ispaused()
{
	return in.paused;
}

static int getlength()
{
	return acm_time_total(in.acm);
}

static int get_time()
{
	int d;
	if (in.seek_to >= 0)
		return in.seek_to;
	d = plugin->outMod->GetWrittenTime() - plugin->outMod->GetOutputTime();
	return acm_time_tell(in.acm) - d;
	/* return plugin->outMod->GetOutputTime(); */
}

static void setoutputtime(int ms)
{
	in.seek_to = ms;
}

static void setvolume(int vol)
{
	plugin->outMod->SetVolume(vol);
}

static void setpan(int pan)
{
	plugin->outMod->SetPan(pan);
}

static int read_and_play(char *buf) {
	int pos_ms, blen, need_len, res, snum;

	/* check if buffer available */
	blen = need_len = SBLOCK * acm_channels(in.acm) * ACM_WORD;
	if (plugin->dsp_isactive())
		need_len *= 2;
	if (plugin->outMod->CanWrite() < need_len) {
		Sleep(20);
		return 0;
	}

	/* load samples */
	res = acm_read_loop(in.acm, buf, blen, 0,2,1);
	if (res <= 0)
		return 1;
	snum = res / (acm_channels(in.acm) * ACM_WORD);
	
	/* vis seems ignored when dsp is on */
	pos_ms = plugin->outMod->GetWrittenTime();
	plugin->SAAddPCMData(buf, acm_channels(in.acm), ACM_WORD*8, pos_ms);
	plugin->VSAAddPCMData(buf, acm_channels(in.acm), ACM_WORD*8, pos_ms);
	
	/* apply effects if needed */
	if (plugin->dsp_isactive()) {
		snum = plugin->dsp_dosamples((short*)buf, snum, ACM_WORD*8,
					     acm_channels(in.acm), acm_rate(in.acm));
	}
	
	/* output */
	plugin->outMod->Write((char*)buf, res);
	return 0;
}

static int try_seeking()
{
	int eof = 1, pos;
	pos = acm_seek_time(in.acm, in.seek_to);
	if (pos >= 0) {
		plugin->outMod->Flush(acm_time_tell(in.acm));
		eof = in.paused = 0;
	}
	in.seek_to = -1;
	return eof;
}

static DWORD WINAPI __stdcall decode_thread(void *arg)
{
	int eof = 0;
	char *buf;

	buf = malloc(SBLOCK * acm_channels(in.acm) * ACM_WORD);

	while (!dec_quit) {
		if (in.seek_to >= 0)
			eof = try_seeking();
		
		if (!eof) {
			eof = read_and_play(buf);
			continue;
		}

		/* eof, wait until buffer empty before notifying */
		plugin->outMod->CanWrite();	
		if (!plugin->outMod->IsPlaying()) {
			PostMessage(plugin->hMainWindow, WM_AMP_EOF, 0, 0);
			free(buf);
			return 0;
		}
		Sleep(10);
	}
	free(buf);
	return 0;
}

static int play(char *fn)
{
	ACMStream *acm;
	DWORD thread_id;
	int err, latency;

	if ((err = acm_open_file(&acm, fn)) < 0)
		return 1;

	latency = plugin->outMod->Open(acm_rate(acm), acm_channels(acm),
			ACM_WORD*8, -1,-1);
	if (latency < 0) {
		/* error opening device */
		acm_close(acm);
		return 1;
	}
	in.acm = acm;
	in.filename = strdup(fn);
	in.paused = 0;
	in.seek_to = -1;

	plugin->SetInfo(acm_bitrate(acm) / 1000,
			acm_rate(acm) / 1000, acm_channels(acm), 1);

	/* initialize vis stuff */
	plugin->SAVSAInit(latency, acm_rate(acm));
	plugin->VSASetInfo(acm_rate(acm), acm_channels(acm));

	/* set the output plug-ins default volume */
	plugin->outMod->SetVolume(-666);

	dec_quit = 0;
	dec_thread = CreateThread(NULL, 0, decode_thread, NULL, 0, &thread_id);
	
	return 0;
}

static void stop()
{
	if (dec_thread != INVALID_HANDLE_VALUE) {
		dec_quit = 1;
		if (WaitForSingleObject(dec_thread,INFINITE) == WAIT_TIMEOUT) {
			MessageBox(plugin->hMainWindow,
					"error asking thread to die!\n",
					"error killing decode thread", 0);
			TerminateThread(dec_thread, 0);
		}
		CloseHandle(dec_thread);
		dec_thread = INVALID_HANDLE_VALUE;
	}
	acm_close(in.acm);
	free(in.filename);

	plugin->outMod->Close();
	plugin->SAVSADeInit();
}

static int file_info_box(char *fn, HWND hwnd)
{
	char buf[1024];
	ACMStream *acm;
	int err, kbps, secs;
	const ACMInfo *inf;
	
	err = acm_open_file(&acm, fn);
	if (err < 0)
		return 1;

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
			"ACM block=%d\n",
			fn, secs/60, secs % 60,
			acm_pcm_total(acm), inf->rate,
			inf->channels, kbps,
			inf->acm_cols, inf->acm_rows,
			inf->acm_cols * inf->acm_rows);
	acm_close(acm);

	MessageBox(hwnd, buf, "InterPlay ACM Audio file", MB_OK);
	return 0;
}

static void about(HWND hwndParent)
{
	MessageBox(hwndParent,
			"InterPlay ACM Audio Decoder - "
			"libacm " LIBACM_VERSION "\n\n"
			"Homepage: http://libacm.berlios.de/\n"
			"\n",
			"About", MB_OK);
}

static In_Module _plugin =
{
	IN_VER,
	"InterPlay ACM Audio Decoder (libacm " LIBACM_VERSION ")",
	0,		/* hMainWindow */
	0,		/* hDllInstance */
	"ACM\0ACM Audio File (*.ACM)\0",
	1,		/* is_seekable */
	1,		/* uses output */
	config,
	about,
	init,
	quit,
	get_song_info,
	file_info_box,
	is_our_file,
	play,
	pause,
	unpause,
	ispaused,
	stop,
	getlength,
	get_time,
	setoutputtime,
	setvolume,
	setpan,

	0,0,0,0,0,0,0,0,0,	/* vis stuff */
	0, 0,		/* dsp */
	NULL,		/* eq_set */
	NULL,		/* setinfo */
	0		/* out_mod */
};

__declspec(dllexport) In_Module *winampGetInModule2()
{
	plugin = &_plugin;
	return plugin;
}

