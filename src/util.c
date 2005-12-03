/*
 * utility funtions for libacm
 *
 * Copyright (C) 2004  Marko Kreen
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

#include <stdio.h>
#include <string.h>

#include "libacm.h"

#define ACM_HEADER_LEN	14

/*
 * error strings
 */
static const char *_errlist[] = {
	"No error",
	"ACM error",
	"Cannot open file",
	"Not an ACM file",
	"Read error",
	"Bad format",
	"Corrupt file",
	"Unexcpected EOF",
	"Stream not seekable"
};

const char *acm_strerror(int err)
{
	int nerr = sizeof(_errlist) / sizeof(char *);
	if ((-err) < 0 || (-err) >= nerr)
		return "Unknown error";
	return _errlist[-err];
}

/*
 * File IO using stdio
 */

static int _read_file(void *ptr, int size, int n, void *arg)
{
	FILE *f = (FILE *)arg;
	return fread(ptr, size, n, f);
}
                                                                                
static int _close_file(void *arg)
{
	FILE *f = (FILE *)arg;
	return fclose(f);
}
                                                                                
static int _seek_file(void *arg, int offset, int whence)
{
	FILE *f = (FILE *)arg;
	return fseek(f, offset, whence);
}

static int _get_length_file(void *arg)
{
	FILE *f = (FILE *)arg;
	int res, pos, len = -1;

	pos = ftell(f);
	if (pos < 0)
		return -1;

	res = fseek(f, 0, SEEK_END);
	if (res >= 0) {
		len = ftell(f);
		fseek(f, pos, SEEK_SET);
	}
	return len;
}

int acm_open_file(ACMStream **res, const char *filename)
{
	int err;
	FILE *f;
	acm_io_callbacks io;
	ACMStream *acm;

	if ((f = fopen(filename, "rb")) == NULL)
       		return ACM_ERR_OPEN;

	memset(&io, 0, sizeof(io));
	io.read_func = _read_file;
	io.seek_func = _seek_file;
	io.close_func = _close_file;
	io.get_length_func = _get_length_file;

	if ((err = acm_open_decoder(&acm, f, io)) < 0) {
		fclose(f);
		return err;
	}
	*res = acm;
	return 0;
}

/*
 * utility functions
 */
static int pcm2time(ACMStream *acm, int pcm)
{
	return ((10 * pcm) / acm->info.rate) * 100;
}

/*
 * info functions
 */

const ACMInfo *acm_info(ACMStream *acm)
{
	return &acm->info;
}

int acm_seekable(ACMStream *acm)
{
	return acm->data_len > 0;
}

int acm_bitrate(ACMStream *acm)
{
	long secs, bitrate;
	const ACMInfo *info = acm_info(acm);

	if (acm_raw_total(acm) < 0)
		return 13000;

	secs = acm_pcm_total(acm) / info->rate;
	bitrate = acm_raw_total(acm) / secs * 8;
	return bitrate;
}

int acm_pcm_tell(ACMStream *acm)
{
	return acm->stream_pos / acm->info.channels;
}

int acm_pcm_total(ACMStream *acm)
{
	return acm->total_values / acm->info.channels;
}

int acm_time_tell(ACMStream *acm)
{
	return pcm2time(acm, acm_pcm_tell(acm));
}

int acm_time_total(ACMStream *acm)
{
	return pcm2time(acm, acm_pcm_total(acm));
}

int acm_raw_tell(ACMStream *acm)
{
	return acm->buf_start_ofs + acm->buf_pos;
}

int acm_raw_total(ACMStream *acm)
{
	return acm->data_len;
}

/*
 * seeking
 */
int acm_seek_time(ACMStream *acm, int time_ms)
{
	int pos_pcm = (time_ms / 100) * (acm->info.rate / 10);
	return acm_seek_pcm(acm, pos_pcm);
}

int acm_seek_pcm(ACMStream *acm, int pcm_pos)
{
	int res, word_pos;

	word_pos = pcm_pos * acm->info.channels;

	if (word_pos < acm->stream_pos) {
		if (acm->io.seek_func == NULL)
			return ACM_ERR_NOT_SEEKABLE;

		if (acm->io.seek_func(acm->io_arg,ACM_HEADER_LEN,SEEK_SET) < 0)
			return ACM_ERR_NOT_SEEKABLE;
		
		acm->buf_pos = 0;
 		acm->buf_size = 0;
		acm->bit_avail = 0;
		acm->bit_data = 0;

		acm->stream_pos = 0;
		acm->block_pos = 0;
		acm->block_ready = 0;
		acm->buf_start_ofs = ACM_HEADER_LEN;

		memset(acm->wrapbuf, 0, acm->wrapbuf_len * sizeof(int));
	}
	while (acm->stream_pos < word_pos) {
		int step = 2048;
		if (acm->stream_pos + step > word_pos)
			step = word_pos - acm->stream_pos;

		res = acm_read(acm, NULL, step*2, 0,2,1);
		if (res < 1)
			break;
	}
	return acm->stream_pos * acm->info.channels;
}

/*
 * read loop - full block reading
 */
int acm_read_loop(ACMStream *acm, char *dst, int bytes,
		int bigendianp, int samplelen, int sgned)
{
	int res, got = 0;
	while (bytes > 0) {
		res = acm_read(acm, dst, bytes, bigendianp, samplelen, sgned);
		if (res > 0) {
			dst += res;
			got += res;
			bytes -= res;
		} else {
			if (res < 0 && got == 0)
				return res;
			break;
		}
	}
	return got;
}

