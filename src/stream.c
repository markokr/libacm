/*
 * Buffering around libsndfile and libsamplerate.
 *
 * Copyright (c) 2026 Marko Kreen
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sndfile.h>
#include <samplerate.h>

#include "stream.h"

#define CONV_MODE (SRC_SINC_BEST_QUALITY)
#define BUF_FRAMES (8 * 1024)
#define BUF_RATIO_LIMIT (4)

struct FloatBuf {
	float *samples;
	int read_pos;
	int write_pos;
	int size;
};

struct Stream {
	SNDFILE *sf;

	int channels;
	int input_rate;
	int output_rate;
	int writing;

	SRC_STATE *conv_state;
	double conv_ratio;
	const char *conv_error;

	struct FloatBuf input;
	struct FloatBuf output;

	short *sf_write_buf;
	int sf_write_buf_size;
};

/*
 * FloatBuf
 */

static float clampf(float f, float min, float max)
{
	if (isnan(f))
		return 0.0;
	if (f < min)
		return min;
	if (f > max)
		return max;
	return f;
}

static int16_t float_to_short(float s)
{
	long n = lrintf(clampf(s, -1.0, 1.0) * (float)0x8000);
	if (n < INT16_MIN) {
		return INT16_MIN;
	} else if (n > INT16_MAX) {
		return INT16_MAX;
	}
	return n;
}

static float short_to_float(short s)
{
	return (float)s / (float)0x8000;
}

static int fbuf_init(struct FloatBuf *buf, int size)
{
	buf->samples = calloc(size, sizeof(*buf->samples));
	if (!buf->samples)
		return -1;
	buf->size = size;
	buf->read_pos = buf->write_pos = 0;
	return 0;
}

static void fbuf_free(struct FloatBuf *buf)
{
	if (buf->samples)
		free(buf->samples);
	buf->samples = NULL;
	buf->read_pos = buf->write_pos = buf->size = 0;
}

static int fbuf_readable(struct FloatBuf *buf)
{
	return buf->write_pos - buf->read_pos;
}

static int fbuf_writable(struct FloatBuf *buf)
{
	return buf->size - buf->write_pos;
}

static void fbuf_reset(struct FloatBuf *buf)
{
	int remain = fbuf_readable(buf);
	if (remain && buf->read_pos) {
		memcpy(buf->samples, &buf->samples[buf->read_pos], remain * sizeof(*buf->samples));
	}
	buf->write_pos = remain;
	buf->read_pos = 0;
}

static int fbuf_read_sample(struct FloatBuf *buf, float *sample)
{
	if (!fbuf_readable(buf))
		return -1;
	*sample = buf->samples[buf->read_pos++];
	return 0;
}

static int fbuf_write_sample(struct FloatBuf *buf, float sample)
{
	if (!fbuf_writable(buf))
		return -1;
	buf->samples[buf->write_pos++] = sample;
	return 0;
}

static int fbuf_receive(struct FloatBuf *buf, SNDFILE *sf)
{
	fbuf_reset(buf);
	int n = fbuf_writable(buf);
	sf_count_t res = sf_read_float(sf, &buf->samples[buf->write_pos], n);
	buf->write_pos += res;
	return 0;
}

static int fbuf_send(struct FloatBuf *buf, SNDFILE *sf, short *tmp, int tmplen)
{
	if (tmp != NULL) {
		/* avoid precision loss bug in sndfile's float->int conversion */
		while (fbuf_readable(buf) > 0) {
			int n = fbuf_readable(buf) > tmplen ? tmplen : fbuf_readable(buf);
			for (int i = 0; i < n; i++) {
				tmp[i] = float_to_short(buf->samples[buf->read_pos + i]);
			}
			sf_count_t res = sf_write_short(sf, tmp, n);
			if (res == 0)
				break;
			buf->read_pos += res;
		}
	} else if (fbuf_readable(buf) > 0) {
		int n = fbuf_readable(buf);
		buf->read_pos += sf_write_float(sf, &buf->samples[buf->read_pos], n);
	}
	fbuf_reset(buf);
	return 0;
}

/*
 * Sample rate converter
 */

static int setup_converter(struct Stream *stream)
{
	int conv_err;
	int in_frames = BUF_FRAMES, out_frames = BUF_FRAMES;

	/* no conversion */
	if (stream->input_rate == stream->output_rate) {
		if (stream->writing)
			return fbuf_init(&stream->input, in_frames * stream->channels);
		return fbuf_init(&stream->output, out_frames * stream->channels);
	}

	stream->conv_ratio = (double)stream->output_rate / (double)stream->input_rate;

	if (stream->conv_ratio > 1.0) {
		out_frames = ceil(in_frames * stream->conv_ratio);
		if (out_frames > BUF_RATIO_LIMIT * in_frames)
			out_frames = BUF_RATIO_LIMIT * in_frames;
	} else {
		in_frames = ceil(out_frames / stream->conv_ratio);
		if (in_frames > BUF_RATIO_LIMIT * out_frames)
			in_frames = BUF_RATIO_LIMIT * out_frames;
	}
	if (fbuf_init(&stream->input, in_frames * stream->channels) < 0)
		return -1;
	if (fbuf_init(&stream->output, out_frames * stream->channels) < 0)
		return -1;

	stream->conv_state = src_new(CONV_MODE, stream->channels, &conv_err);
	if (!stream->conv_state) {
		stream->conv_error = src_strerror(conv_err);
		return -1;
	}

	return 0;
}

static int convert_rate(struct Stream *stream, int final)
{
	struct FloatBuf *inp = &stream->input;
	struct FloatBuf *out = &stream->output;
	SRC_DATA data;

	fbuf_reset(out);

	memset(&data, 0, sizeof(data));

	data.src_ratio = stream->conv_ratio;
	data.end_of_input = final;

	data.data_in = &inp->samples[inp->read_pos];
	data.input_frames = fbuf_readable(inp) / stream->channels;

	data.data_out = &out->samples[out->write_pos];
	data.output_frames = fbuf_writable(out) / stream->channels;

	int err = src_process(stream->conv_state, &data);
	if (err != 0) {
		stream->conv_error = src_strerror(err);
		return -1;
	}

	inp->read_pos += data.input_frames_used * stream->channels;
	out->write_pos += data.output_frames_gen * stream->channels;

	fbuf_reset(inp);

	return 0;
}

static int read_and_convert(struct Stream *stream)
{
	struct FloatBuf *inp = &stream->input;
	struct FloatBuf *out = &stream->output;

	/* no conversion */
	if (!stream->conv_state) {
		return fbuf_receive(out, stream->sf);
	}

	/* fill src_process() buffers */
	do {
		if (fbuf_writable(inp) > 0) {
			if (fbuf_receive(inp, stream->sf) < 0)
				return -1;
		}
		int final = fbuf_readable(inp) == 0;
		if (convert_rate(stream, final) < 0)
			return -1;
	} while (fbuf_readable(out) == 0);

	return 0;
}

static int write_flush(struct Stream *stream, int final)
{
	struct FloatBuf *inp = &stream->input;
	struct FloatBuf *out = &stream->output;

	if (!stream->writing || !stream->sf)
		return 0;

	if (stream->conv_state == NULL) {
		if (inp->samples) {
			return fbuf_send(inp, stream->sf, stream->sf_write_buf,
					 stream->sf_write_buf_size);
		}
		return 0;
	}

	/* drain src_process() buffers if final */
	do {
		int has_input = fbuf_readable(inp) > 0;

		int err = convert_rate(stream, final);
		if (err < 0)
			return err;

		int has_output = fbuf_readable(out) > 0;

		err = fbuf_send(out, stream->sf, stream->sf_write_buf, stream->sf_write_buf_size);
		if (err < 0)
			return err;

		if (!has_input && !has_output)
			break;
	} while (final);

	return 0;
}

/*
 * Public API
 */

struct Stream *stream_sf_open_read(SNDFILE *sf, int output_rate)
{
	SF_INFO info;

	memset(&info, 0, sizeof(info));
	int err = sf_command(sf, SFC_GET_CURRENT_SF_INFO, &info, sizeof(info));
	if (err) {
		sf_close(sf);
		return NULL;
	}

	struct Stream *stream = calloc(1, sizeof(struct Stream));
	if (!stream) {
		sf_close(sf);
		return NULL;
	}
	stream->sf = sf;
	stream->channels = info.channels;
	stream->input_rate = info.samplerate;
	stream->output_rate = output_rate > 0 ? output_rate : stream->input_rate;
	stream->writing = 0;

	if (setup_converter(stream) < 0) {
		stream_close(stream);
		return NULL;
	}

	return stream;
}

struct Stream *stream_sf_open_write(SNDFILE *sf, int input_rate)
{
	SF_INFO info;
	memset(&info, 0, sizeof(info));
	int err = sf_command(sf, SFC_GET_CURRENT_SF_INFO, &info, sizeof(info));
	if (err) {
		sf_close(sf);
		return NULL;
	}
	struct Stream *stream = calloc(1, sizeof(struct Stream));
	if (!stream) {
		sf_close(sf);
		return NULL;
	}
	stream->sf = sf;
	stream->channels = info.channels;
	stream->input_rate = input_rate > 0 ? input_rate : info.samplerate;
	stream->output_rate = info.samplerate;
	stream->writing = 1;

	stream->sf_write_buf_size = BUF_FRAMES * stream->channels;
	stream->sf_write_buf = calloc(stream->sf_write_buf_size, sizeof(short));

	if (setup_converter(stream) < 0) {
		stream_close(stream);
		return NULL;
	}

	return stream;
}

struct Stream *stream_open_read(const char *fn, int output_rate)
{
	SF_INFO info;
	memset(&info, 0, sizeof(info));

	SNDFILE *sf = sf_open(fn, SFM_READ, &info);
	if (!sf)
		return NULL;
	return stream_sf_open_read(sf, output_rate);
}

struct Stream *stream_open_write(const char *fn, int nchan, int input_rate, int output_rate)
{
	SF_INFO info;
	memset(&info, 0, sizeof(info));
	info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	info.channels = nchan;
	info.samplerate = output_rate > 0 ? output_rate : input_rate;

	SNDFILE *sf = sf_open(fn, SFM_WRITE, &info);
	if (!sf)
		return NULL;
	return stream_sf_open_write(sf, input_rate);
}

int stream_channels(struct Stream *stream)
{
	return stream->channels;
}

int stream_input_rate(struct Stream *stream)
{
	return stream->input_rate;
}

int stream_output_rate(struct Stream *stream)
{
	return stream->output_rate;
}

int stream_read_float(struct Stream *stream, float *sample)
{
	struct FloatBuf *out = &stream->output;
	if (stream->writing)
		return -1;
	if (fbuf_readable(out) == 0) {
		if (read_and_convert(stream) < 0)
			return -1;
	}
	return fbuf_read_sample(out, sample);
}

int stream_write_float(struct Stream *stream, float sample)
{
	struct FloatBuf *inp = &stream->input;
	if (!stream->writing)
		return -1;
	if (fbuf_writable(inp) == 0) {
		if (write_flush(stream, 0) < 0)
			return -1;
	}
	return fbuf_write_sample(inp, sample);
}

int stream_read_short(struct Stream *stream, short *sample)
{
	float f;
	int err = stream_read_float(stream, &f);
	if (err == 0) {
		*sample = float_to_short(f);
	}
	return err;
}

int stream_write_short(struct Stream *stream, short sample)
{
	return stream_write_float(stream, short_to_float(sample));
}

void stream_close(struct Stream *stream)
{
	write_flush(stream, 1);
	if (stream->sf)
		sf_close(stream->sf);
	if (stream->conv_state)
		src_delete(stream->conv_state);
	if (stream->sf_write_buf)
		free(stream->sf_write_buf);
	fbuf_free(&stream->input);
	fbuf_free(&stream->output);
	free(stream);
}

void stream_perror(struct Stream *stream, const char *desc)
{
	SNDFILE *sf = stream ? stream->sf : NULL;
	if (stream && stream->conv_error) {
		fprintf(stderr, "%s: %s\n", desc, stream->conv_error);
	} else if (sf_error(sf) != 0) {
		fprintf(stderr, "%s: %s\n", desc, sf_strerror(sf));
	} else if (errno != 0) {
		perror(desc);
	} else {
		fprintf(stderr, "%s: No error?\n", desc);
	}
}
