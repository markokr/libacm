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

#ifndef _STREAM_H_
#define _STREAM_H_

#include <sndfile.h>

/** Audio stream */
struct Stream;

/**
 * Pick type for new file based on type description or filename extension.
 *
 * Returns libsndfile format code or 0 if failed.
 */
int stream_guess_format(const char *type, const char *fn);

/** Open existing audio file for reading, with optional rate conversion. */
struct Stream *stream_open_read(const char *fn, int output_rate);

/**
 * Create new audio file for writing, with optional rate conversion.
 *
 * 'format' is sndfile format code.
 */
struct Stream *stream_open_write(const char *fn, int format, int nchan, int input_rate,
				 int output_rate);

/** Wrap existing SNDFILE for reading, takes ownership */
struct Stream *stream_open_read_sf(SNDFILE *sf, int output_rate);

/** Wrap existing SNDFILE for writing, takes ownership */
struct Stream *stream_open_write_sf(SNDFILE *sf, int input_rate);

/**
 * Set libsndfile compression level [0.0..1.0] for flac/mp3/opus/...
 *
 * - 0.0: less compression, higher quality
 * - 1.0: more compression, lower quality
 */
int stream_set_compression_level(struct Stream *stream, double level);

/** Close file and free memory */
void stream_close(struct Stream *stream);

/** Return number of channels in audio stream */
int stream_channels(struct Stream *stream);

/** Return sample rate of incoming audio stream */
int stream_input_rate(struct Stream *stream);

/** Return sample rate of outgoing audio stream */
int stream_output_rate(struct Stream *stream);

/** Read 16-bit signed integer value */
int stream_read_short(struct Stream *stream, short *sample);

/** Read 32-bit signed integer value */
int stream_read_int(struct Stream *stream, int *sample);

/** Read 32-bit float value in range [-1.0 .. +1.0] */
int stream_read_float(struct Stream *stream, float *sample);

/** Write 16-bit signed integer value */
int stream_write_short(struct Stream *stream, short sample);

/** Read 32-bit signed integer value */
int stream_write_int(struct Stream *stream, int sample);

/** Write 32-bit float value in range [-1.0 .. +1.0] */
int stream_write_float(struct Stream *stream, float sample);

/** Return error string or NULL */
const char *stream_error(struct Stream *stream);

/** Write error string to stderr */
void stream_perror(struct Stream *stream, const char *desc);

#endif
