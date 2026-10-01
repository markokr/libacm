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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sndfile.h>

#ifdef HAVE_SAMPLERATE
#include <samplerate.h>
#endif

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

	struct FloatBuf input;
	struct FloatBuf output;

	int channels;
	int input_rate;
	int output_rate;
	int writing;

#ifdef HAVE_SAMPLERATE
	SRC_STATE *conv_state;
	double conv_ratio;
#endif
	const char *conv_error;
};

#ifdef HAVE_SAMPLERATE
#define DO_CONVERT(stream) ((stream)->conv_state != NULL)
#else
#define DO_CONVERT(stream) (0)
#endif

static const char *last_error = NULL;

static int set_error(struct Stream *stream, const char *error)
{
	if (stream)
		stream->conv_error = error;
	last_error = error;
	return -1;
}

/*
 * Float conversion
 */

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define CLAMP(v, min, max) ((v) > (max) ? (max) : ((v) < (min) ? (min) : (v)))

static inline float clampf(float f, float min, float max)
{
	return isnan(f) ? 0.0f : CLAMP(f, min, max);
}

/* max X that: X * factor < factor */
#define FLOAT_MAX(factor) (1.0f - (1.0f / (float)MIN(factor, 1LL << 24)))

/* convert [-factor..+factor) => [-1.0..+1.0) */
#define FLOAT_NORMALIZE(s, factor) ((float)(s) / (float)(factor))
/* convert [-1.0..+1.0) => [-factor..+factor) */
#define FLOAT_DENORMALIZE(s, factor) (clampf(s, -1.0f, FLOAT_MAX(factor)) * (float)(factor))

#define S16_FACTOR (1LL << 15)
#define S32_FACTOR (1LL << 31)

static inline float short_to_float(short s)
{
	return FLOAT_NORMALIZE(s, S16_FACTOR);
}

static inline short float_to_short(float s)
{
	return lrintf(FLOAT_DENORMALIZE(s, S16_FACTOR));
}

static inline float int_to_float(short s)
{
	return FLOAT_NORMALIZE(s, S32_FACTOR);
}

static inline int float_to_int(float s)
{
	return FLOAT_DENORMALIZE(s, S32_FACTOR);
}

/*
 * FloatBuf
 */

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
	buf->read_pos = buf->write_pos = 0;
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

#define FBUF_SEND_TYPE(fbuf, channels, float_converter, sfwriter, stype) \
	do { \
		stype sbuf[1024]; /* SF_MAX_CHANNELS=1024 */ \
		int frames = sizeof(sbuf) / (sizeof(stype) * (channels)); \
		while (fbuf_readable(fbuf) > 0) { \
			int n = MIN(fbuf_readable(fbuf), frames * (channels)); \
			for (int i = 0; i < n; i++) { \
				sbuf[i] = float_converter((fbuf)->samples[(fbuf)->read_pos + i]); \
			} \
			sf_count_t res = sfwriter(sf, sbuf, n); \
			if (res == 0) \
				break; \
			buf->read_pos += res; \
		} \
	} while (0)

static int fbuf_send(struct FloatBuf *buf, SNDFILE *sf)
{
	SF_INFO info;
	if (sf_command(sf, SFC_GET_CURRENT_SF_INFO, &info, sizeof(info)) != 0)
		return -1;

	/* avoid incorrect float->int conversion in sndfile */
	switch (info.format & SF_FORMAT_SUBMASK) {
	default:
	case SF_FORMAT_PCM_16:
		FBUF_SEND_TYPE(buf, info.channels, float_to_short, sf_write_short, short);
		break;
	case SF_FORMAT_PCM_24:
	case SF_FORMAT_PCM_32:
	case SF_FORMAT_ALAC_20:
	case SF_FORMAT_ALAC_24:
	case SF_FORMAT_ALAC_32:
		FBUF_SEND_TYPE(buf, info.channels, float_to_int, sf_write_int, int);
		break;
	case SF_FORMAT_FLOAT:
	case SF_FORMAT_DOUBLE:
		int n = fbuf_readable(buf);
		buf->read_pos += sf_write_float(sf, &buf->samples[buf->read_pos], n);
		break;
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
	int limit = BUF_FRAMES * stream->channels * BUF_RATIO_LIMIT;

	/* no conversion */
	if (stream->input_rate == stream->output_rate) {
		if (stream->writing)
			return fbuf_init(&stream->input, in_frames * stream->channels);
		return fbuf_init(&stream->output, out_frames * stream->channels);
	}
#ifdef HAVE_SAMPLERATE
	stream->conv_ratio = (double)stream->output_rate / (double)stream->input_rate;
	if (stream->conv_ratio > 1.0) {
		out_frames = MIN(ceil(in_frames * stream->conv_ratio), limit);
	} else {
		in_frames = MIN(ceil(out_frames * stream->conv_ratio), limit);
	}
	if (fbuf_init(&stream->input, in_frames * stream->channels) < 0)
		return -1;
	if (fbuf_init(&stream->output, out_frames * stream->channels) < 0)
		return -1;
	stream->conv_state = src_new(CONV_MODE, stream->channels, &conv_err);
	if (!stream->conv_state)
		return set_error(stream, src_strerror(conv_err));
	return 0;
#else
	return set_error(stream, "libsamplerate not available");
#endif
}

static int convert_rate(struct Stream *stream, int final)
{
#ifdef HAVE_SAMPLERATE
	struct FloatBuf *inp = &stream->input;
	struct FloatBuf *out = &stream->output;
	SRC_DATA data;

	fbuf_reset(out);

	memset(&data, 0, sizeof(data));

	data.src_ratio = stream->conv_ratio;
	data.end_of_input = final;

	data.data_in = &inp->samples[inp->read_pos];
	data.data_out = &out->samples[out->write_pos];
	data.input_frames = fbuf_readable(inp) / stream->channels;
	data.output_frames = fbuf_writable(out) / stream->channels;

	int err = src_process(stream->conv_state, &data);
	if (err != 0) {
		return set_error(stream, src_strerror(err));
	}

	inp->read_pos += data.input_frames_used * stream->channels;
	out->write_pos += data.output_frames_gen * stream->channels;

	fbuf_reset(inp);

	return 0;
#else
	return -1;
#endif
}

static int read_and_convert(struct Stream *stream)
{
	struct FloatBuf *out = &stream->output;
	struct FloatBuf *inp = &stream->input;

	if (DO_CONVERT(stream)) {
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

	/* no conversion */
	return fbuf_receive(out, stream->sf);
}

static int write_flush(struct Stream *stream, int final)
{
	struct FloatBuf *inp = &stream->input;

	if (!stream->writing || !stream->sf)
		return 0;

	if (DO_CONVERT(stream)) {
		struct FloatBuf *out = &stream->output;
		/* drain src_process() buffers if final */
		do {
			int has_input = fbuf_readable(inp) > 0;

			int err = convert_rate(stream, final);
			if (err < 0)
				return err;

			int has_output = fbuf_readable(out) > 0;

			err = fbuf_send(out, stream->sf);
			if (err < 0)
				return err;

			if (!has_input && !has_output)
				break;
		} while (final);

	} else if (inp->samples) {
		return fbuf_send(inp, stream->sf);
	}
	return 0;
}

struct LookupCode {
	const char *code;
	int value;
};

static const struct LookupCode SAMPLE_FMT_MAP[] = {
	{ "u8", SF_FORMAT_PCM_U8 },
	{ "s8", SF_FORMAT_PCM_S8 },
	{ "s16", SF_FORMAT_PCM_16 },
	{ "s24", SF_FORMAT_PCM_24 },
	{ "s32", SF_FORMAT_PCM_32 },
	/* float */
	{ "f32", SF_FORMAT_FLOAT },
	{ "f64", SF_FORMAT_DOUBLE },
	/* weird */
	{ "alaw", SF_FORMAT_ALAW },
	{ "ulaw", SF_FORMAT_ULAW },
	{ NULL, 0 },
};

static const struct LookupCode FILE_FMT_MAP[] = {
	/* old 32-bit formats */
	{ "aiff", SF_FORMAT_AIFF | SF_FORMAT_PCM_16 },
	{ "au", SF_FORMAT_AU | SF_FORMAT_PCM_16 },
	{ "w32", SF_FORMAT_WAV | SF_FORMAT_PCM_16 },
	/* newer formats */
	{ "caf", SF_FORMAT_CAF | SF_FORMAT_PCM_16 },
	{ "wav", SF_FORMAT_RF64 | SF_FORMAT_PCM_16 },
	{ "w64", SF_FORMAT_W64 | SF_FORMAT_PCM_16 },
	/* compressed */
	{ "alac", SF_FORMAT_CAF | SF_FORMAT_ALAC_16 },
	{ "flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_16 },
#ifdef HAVE_SNDFILE_1_1
	{ "mp3", SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III },
	{ "ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS },
	{ "opus", SF_FORMAT_OGG | SF_FORMAT_OPUS },
#endif
	{ NULL, 0 },
};

static int lookup_code(const struct LookupCode *lookup, const char *str)
{
	for (; lookup->code != NULL; lookup++) {
		if (strcasecmp(str, lookup->code) == 0) {
			return lookup->value;
		}
	}
	return 0;
}

static int format_fixups(int format, int subformat)
{
	int type = format & SF_FORMAT_TYPEMASK;
	int sub = format & SF_FORMAT_SUBMASK;

	switch (type) {
	case SF_FORMAT_CAF:
		if (sub == SF_FORMAT_ALAC_16 && subformat) {
			switch (subformat) {
			case SF_FORMAT_PCM_16:
				subformat = SF_FORMAT_ALAC_16;
				break;
			case SF_FORMAT_PCM_24:
				subformat = SF_FORMAT_ALAC_24;
				break;
			case SF_FORMAT_PCM_32:
				subformat = SF_FORMAT_ALAC_32;
				break;
			}
		}
		break;
	case SF_FORMAT_WAV:
		if (subformat && subformat != SF_FORMAT_PCM_16)
			format = (format ^ SF_FORMAT_WAV) | SF_FORMAT_WAVEX;
		break;
	}

	if (format && subformat)
		format = (format ^ sub) | subformat;

	return format;
}

/*
 * Public API
 */

int stream_guess_format(const char *type, const char *fn)
{
	char tmp[8];
	int subformat = 0;
	int format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

	const char *ext = fn ? strrchr(fn, '.') : NULL;
	if (!type && ext)
		type = ext;
	if (!type)
		return format;

	const char *sep = strchr(type, ':');
	if (sep && sep - type < (int)sizeof(tmp)) {
		memset(tmp, 0, sizeof(tmp));
		memcpy(tmp, type, sep - type);
		format = lookup_code(FILE_FMT_MAP, tmp);
		subformat = lookup_code(SAMPLE_FMT_MAP, sep + 1);
		if (!subformat)
			return 0;
	} else {
		format = lookup_code(FILE_FMT_MAP, type);
	}

	return format_fixups(format, subformat);
}

struct Stream *stream_open_read_sf(SNDFILE *sf, int output_rate)
{
	SF_INFO info;

	memset(&info, 0, sizeof(info));
	int err = sf_command(sf, SFC_GET_CURRENT_SF_INFO, &info, sizeof(info));
	if (err) {
		set_error(NULL, sf_strerror(sf));
		sf_close(sf);
		return NULL;
	}

	struct Stream *stream = calloc(1, sizeof(struct Stream));
	if (!stream) {
		set_error(NULL, strerror(errno));
		sf_close(sf);
		return NULL;
	}
	stream->sf = sf;
	stream->channels = info.channels;
	stream->input_rate = info.samplerate;
	stream->output_rate = output_rate > 0 ? output_rate : stream->input_rate;
	stream->writing = 0;
	last_error = NULL;

	if (setup_converter(stream) < 0) {
		stream_close(stream);
		return NULL;
	}

	return stream;
}

struct Stream *stream_open_write_sf(SNDFILE *sf, int input_rate)
{
	SF_INFO info;

	memset(&info, 0, sizeof(info));
	int err = sf_command(sf, SFC_GET_CURRENT_SF_INFO, &info, sizeof(info));
	if (err) {
		set_error(NULL, sf_strerror(sf));
		sf_close(sf);
		return NULL;
	}

	struct Stream *stream = calloc(1, sizeof(struct Stream));
	if (!stream) {
		set_error(NULL, strerror(errno));
		sf_close(sf);
		return NULL;
	}
	stream->sf = sf;
	stream->channels = info.channels;
	stream->input_rate = input_rate > 0 ? input_rate : info.samplerate;
	stream->output_rate = info.samplerate;
	stream->writing = 1;
	last_error = NULL;

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
	if (!sf) {
		set_error(NULL, sf_strerror(sf));
		return NULL;
	}
	return stream_open_read_sf(sf, output_rate);
}

struct Stream *stream_open_write(const char *fn, int format, int nchan, int input_rate,
				 int output_rate)
{
	SF_INFO info;

	memset(&info, 0, sizeof(info));
	info.channels = nchan;
	info.samplerate = output_rate > 0 ? output_rate : input_rate;
	info.format = format;

	SNDFILE *sf = sf_open(fn, SFM_WRITE, &info);
	if (!sf) {
		set_error(NULL, sf_strerror(sf));
		return NULL;
	}
	if ((format & SF_FORMAT_TYPEMASK) == SF_FORMAT_RF64) {
		sf_command(sf, SFC_RF64_AUTO_DOWNGRADE, NULL, SF_TRUE);
	}
	return stream_open_write_sf(sf, input_rate);
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

int stream_read_int(struct Stream *stream, int *sample)
{
	float f;
	int err = stream_read_float(stream, &f);
	if (err == 0) {
		*sample = float_to_int(f);
	}
	return err;
}

int stream_write_int(struct Stream *stream, int sample)
{
	return stream_write_float(stream, int_to_float(sample));
}

void stream_close(struct Stream *stream)
{
	write_flush(stream, 1);
	if (stream->sf)
		sf_close(stream->sf);
#ifdef HAVE_SAMPLERATE
	if (stream->conv_state)
		src_delete(stream->conv_state);
#endif
	fbuf_free(&stream->input);
	fbuf_free(&stream->output);
	free(stream);
}

const char *stream_error(struct Stream *stream)
{
	if (!stream)
		return last_error;
	if (stream->conv_error)
		return stream->conv_error;
	if (sf_error(stream->sf) != 0)
		return sf_strerror(stream->sf);
	if (errno != 0)
		return strerror(errno);
	return NULL;
}

void stream_perror(struct Stream *stream, const char *desc)
{
	const char *str = stream_error(stream);
	fprintf(stderr, "%s: %s\n", desc, str ? str : "no error");
}
