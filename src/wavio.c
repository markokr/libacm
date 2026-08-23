#include <stdlib.h>
#include <string.h>

#include "wavio.h"

#define FMT_SIZE 16

/* disallow >32bit file ofs */
#define MAX_RIFF_SIZE (0xFFFFFFFF - 8)
#define MAX_DATA_SIZE (MAX_RIFF_SIZE - 8 - FMT_SIZE - 8)

struct Format {
	uint16_t audio_format;
	uint16_t nchannels;
	uint32_t sample_rate;
	uint32_t bytes_per_second;
	uint16_t bytes_per_tick;
	uint16_t bits_per_sample;
};

static bool read_tag(struct WavFile *wav, char tag[4])
{
	return fread(tag, 1, 4, wav->f) == 4;
}

static bool read_u16(struct WavFile *wav, uint16_t *out)
{
	uint8_t buf[2];
	if (fread(buf, 1, 2, wav->f) != 2)
		return false;
	*out = ((uint16_t)(buf[1]) << 8) | buf[0];
	return true;
}

static bool read_s16(struct WavFile *wav, int16_t *out)
{
	uint16_t buf;
	if (!read_u16(wav, &buf))
		return false;
	*out = (int16_t)buf;
	return true;
}

static bool read_u32(struct WavFile *wav, uint32_t *out)
{
	uint8_t buf[4];
	if (fread(buf, 1, 4, wav->f) != 4)
		return false;
	*out = ((uint32_t)buf[3] << 24) | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[1] << 8)
	       | (uint32_t)buf[0];
	return true;
}

static bool read_header(struct WavFile *wav, char tag[4], uint32_t *size)
{
	if (!read_tag(wav, tag))
		return false;
	return read_u32(wav, size);
}

static bool skip_block(struct WavFile *wav, size_t n)
{
	if (n == 0)
		return true;
	return fseek(wav->f, n + (n & 1), SEEK_CUR) == 0;
}

static bool write_tag(struct WavFile *wav, char tag[4])
{
	return fwrite(tag, 1, 4, wav->f) == 4;
}

static bool write_u16(struct WavFile *wav, uint16_t val)
{
	uint8_t buf[2] = { val & 255, val >> 8 };
	return fwrite(buf, 1, 2, wav->f) == 2;
}

static bool write_u32(struct WavFile *wav, uint32_t val)
{
	uint8_t buf[4] = { val & 255, (val >> 8) & 255, (val >> 16) & 255, (val >> 24) & 255 };
	return fwrite(buf, 1, 4, wav->f) == 4;
}

static bool write_header(struct WavFile *wav, char tag[4], uint32_t size)
{
	if (!write_tag(wav, tag))
		return false;
	return write_u32(wav, size);
}

static void fill_format(struct Format *fmt, uint16_t nchan, uint32_t rate)
{
	fmt->audio_format = 1;
	fmt->bits_per_sample = 16;
	fmt->nchannels = nchan;
	fmt->sample_rate = rate;
	fmt->bytes_per_tick = fmt->bits_per_sample * fmt->nchannels / 8;
	fmt->bytes_per_second = fmt->bytes_per_tick * fmt->sample_rate;
}

static bool read_format(struct WavFile *wav, uint32_t size)
{
	struct Format fmt;

	if (size < FMT_SIZE)
		return false;
	if (!read_u16(wav, &fmt.audio_format))
		return false;
	if (!read_u16(wav, &fmt.nchannels))
		return false;
	if (!read_u32(wav, &fmt.sample_rate))
		return false;
	if (!read_u32(wav, &fmt.bytes_per_second))
		return false;
	if (!read_u16(wav, &fmt.bytes_per_tick))
		return false;
	if (!read_u16(wav, &fmt.bits_per_sample))
		return false;
	if (!skip_block(wav, size - FMT_SIZE))
		return false;

	if (fmt.audio_format != 1 || fmt.bits_per_sample != 16)
		return false;
	if (fmt.nchannels == 0 || fmt.sample_rate == 0)
		return false;

	wav->channels = fmt.nchannels;
	wav->sample_rate = fmt.sample_rate;

	return true;
}

static bool write_format(struct WavFile *wav, const struct Format *fmt)
{
	if (!write_header(wav, "fmt ", FMT_SIZE))
		return false;
	if (!write_u16(wav, fmt->audio_format))
		return false;
	if (!write_u16(wav, fmt->nchannels))
		return false;
	if (!write_u32(wav, fmt->sample_rate))
		return false;
	if (!write_u32(wav, fmt->bytes_per_second))
		return false;
	if (!write_u16(wav, fmt->bytes_per_tick))
		return false;
	if (!write_u16(wav, fmt->bits_per_sample))
		return false;
	return true;
}

static bool setup_reader(struct WavFile *wav)
{
	char tag[4];

	uint32_t riff_size, chunk_size;
	bool have_fmt = false;

	if (!read_header(wav, tag, &riff_size) || memcmp(tag, "RIFF", 4) != 0)
		return false;
	if (riff_size > MAX_RIFF_SIZE)
		return false;
	wav->riff_size_ofs = ftell(wav->f) - 4;
	wav->end_ofs = wav->riff_size_ofs + riff_size + 4;
	if (wav->end_ofs < riff_size)
		return false;

	if (!read_tag(wav, tag) || memcmp(tag, "WAVE", 4) != 0)
		return false;

	while (ftell(wav->f) < wav->end_ofs) {
		if (!read_header(wav, tag, &chunk_size))
			return false;

		if (memcmp(tag, "fmt ", 4) == 0) {
			if (!read_format(wav, chunk_size))
				return false;
			have_fmt = true;
		} else if (memcmp(tag, "data", 4) == 0) {
			if (!have_fmt || chunk_size > MAX_DATA_SIZE)
				return false;
			wav->read_ofs = ftell(wav->f);
			wav->data_size = chunk_size;
			wav->data_size_ofs = wav->read_ofs - 4;
			uint32_t data_end_ofs = wav->read_ofs + wav->data_size;
			if (data_end_ofs < wav->end_ofs)
				wav->end_ofs = data_end_ofs;
			if (wav->end_ofs < chunk_size)
				return false;
			return true;
		} else {
			if (!skip_block(wav, chunk_size))
				return false;
		}
	}
	return false;
}

static bool setup_writer(struct WavFile *wav, const struct Format *fmt)
{
	if (!write_header(wav, "RIFF", 0))
		return false;
	wav->riff_size_ofs = ftell(wav->f) - 4;
	if (!write_tag(wav, "WAVE"))
		return false;
	if (!write_format(wav, fmt))
		return false;
	if (!write_header(wav, "data", 0))
		return false;
	wav->data_size_ofs = ftell(wav->f) - 4;
	return true;
}

/*
 * Public API
 */

struct WavFile *wav_open_reader(const char *fn)
{
	struct WavFile *wav = calloc(1, sizeof(struct WavFile));
	if (!wav)
		return NULL;
	wav->f = fopen(fn, "rb");
	if (!wav->f) {
		free(wav);
		return NULL;
	}
	if (!setup_reader(wav)) {
		wav_close(wav);
		return NULL;
	}
	return wav;
}

bool wav_read_sample(struct WavFile *wav, int16_t *sample)
{
	if (wav->read_ofs + 2 > wav->end_ofs)
		return false;

	if (!read_s16(wav, sample))
		return false;
	wav->read_ofs += 2;
	return true;
}

struct WavFile *wav_open_writer(const char *fn, uint16_t nchan, uint32_t rate)
{
	struct Format fmt;
	struct WavFile *wav = calloc(1, sizeof(struct WavFile));
	if (!wav)
		return NULL;
	wav->f = fopen(fn, "wb");
	if (!wav->f) {
		free(wav);
		return NULL;
	}

	fill_format(&fmt, nchan, rate);

	if (!setup_writer(wav, &fmt)) {
		wav_close(wav);
		return NULL;
	}
	return wav;
}

bool wav_write_data(struct WavFile *wav, const void *data, size_t nbytes)
{
	return fwrite(data, 1, nbytes, wav->f) == nbytes;
}

bool wav_write_finish(struct WavFile *wav)
{
	uint32_t data_size = ftell(wav->f) - wav->data_size_ofs - 4;
	uint32_t riff_size = data_size + wav->data_size_ofs - wav->riff_size_ofs;
	if (fseek(wav->f, wav->riff_size_ofs, SEEK_SET) != 0)
		return false;
	if (!write_u32(wav, riff_size))
		return false;
	if (fseek(wav->f, wav->data_size_ofs, SEEK_SET) != 0)
		return false;
	if (!write_u32(wav, data_size))
		return false;
	if (fseek(wav->f, 0, SEEK_END) != 0)
		return false;
	return true;
}

void wav_close(struct WavFile *wav)
{
	if (wav->f)
		fclose(wav->f);
	free(wav);
}
