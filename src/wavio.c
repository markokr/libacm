#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wavio.h"

#define WAVE_FORMAT_PCM 1
#define BITS_PER_SAMPLE 16

#define FMT_SIZE 16

struct Format {
	uint16_t audio_format;
	uint16_t nchannels;
	uint32_t sample_rate;
	uint32_t bytes_per_second;
	uint16_t block_align;
	uint16_t bits_per_sample;
};

struct WavFile {
	FILE *f;

	struct Format fmt;

	/* used when reading */
	uint32_t data_remain;

	/* used when writing */
	uint32_t riff_size_ofs;
	uint32_t data_size_ofs;
};

#define U16(a, b) (((uint16_t)(b) << 8) | (uint16_t)(a))
#define U32(a, b, c, d) \
	(((uint32_t)(d) << 24) | ((uint32_t)(c) << 16) | ((uint32_t)(b) << 8) | (uint32_t)(a))

#define TAG_RIFF U32('R', 'I', 'F', 'F')
#define TAG_WAVE U32('W', 'A', 'V', 'E')
#define TAG_FMT U32('f', 'm', 't', ' ')
#define TAG_DATA U32('d', 'a', 't', 'a')

static bool read_tag(struct WavFile *wf, char tag[4])
{
	return fread(tag, 1, 4, wf->f) == 4;
}

static bool read_u16(struct WavFile *wf, uint16_t *out)
{
	uint8_t buf[2];
	if (fread(buf, 1, 2, wf->f) != 2)
		return false;
	*out = U16(buf[0], buf[1]);
	return true;
}

static bool read_u32(struct WavFile *wf, uint32_t *out)
{
	uint8_t buf[4];
	if (fread(buf, 1, 4, wf->f) != 4)
		return false;
	*out = U32(buf[0], buf[1], buf[2], buf[3]);
	return true;
}

static bool read_header(struct WavFile *wf, uint32_t *tag, uint32_t *size)
{
	if (!read_u32(wf, tag))
		return false;
	return read_u32(wf, size);
}

static bool skip_block(struct WavFile *wf, size_t n)
{
	if (n == 0)
		return true;
	return fseek(wf->f, n + (n & 1), SEEK_CUR) == 0;
}

static bool write_u16(struct WavFile *wf, uint16_t val)
{
	uint8_t buf[2] = { val & 255, val >> 8 };
	return fwrite(buf, 1, 2, wf->f) == 2;
}

static bool write_u32(struct WavFile *wf, uint32_t val)
{
	uint8_t buf[4] = { val & 255, (val >> 8) & 255, (val >> 16) & 255, (val >> 24) & 255 };
	return fwrite(buf, 1, 4, wf->f) == 4;
}

static bool write_header(struct WavFile *wf, uint32_t tag, uint32_t size)
{
	if (!write_u32(wf, tag))
		return false;
	return write_u32(wf, size);
}

static bool write_final_size(struct WavFile *wf, uint32_t ofs, uint32_t size)
{
	if (fseek(wf->f, ofs, SEEK_SET) != 0)
		return false;
	if (!write_u32(wf, size))
		return false;
	return true;
}

static bool read_format(struct WavFile *wf, uint32_t size)
{
	struct Format *fmt = &wf->fmt;

	if (size < FMT_SIZE)
		return false;
	if (!read_u16(wf, &fmt->audio_format))
		return false;
	if (!read_u16(wf, &fmt->nchannels))
		return false;
	if (!read_u32(wf, &fmt->sample_rate))
		return false;
	if (!read_u32(wf, &fmt->bytes_per_second))
		return false;
	if (!read_u16(wf, &fmt->block_align))
		return false;
	if (!read_u16(wf, &fmt->bits_per_sample))
		return false;
	if (!skip_block(wf, size - FMT_SIZE))
		return false;

	if (fmt->audio_format != WAVE_FORMAT_PCM || fmt->bits_per_sample != BITS_PER_SAMPLE)
		return false;
	if (fmt->nchannels == 0 || fmt->sample_rate == 0)
		return false;

	return true;
}

static bool write_format(struct WavFile *wf)
{
	if (!write_header(wf, TAG_FMT, FMT_SIZE))
		return false;
	if (!write_u16(wf, wf->fmt.audio_format))
		return false;
	if (!write_u16(wf, wf->fmt.nchannels))
		return false;
	if (!write_u32(wf, wf->fmt.sample_rate))
		return false;
	if (!write_u32(wf, wf->fmt.bytes_per_second))
		return false;
	if (!write_u16(wf, wf->fmt.block_align))
		return false;
	if (!write_u16(wf, wf->fmt.bits_per_sample))
		return false;
	return true;
}

static void fill_format(struct Format *fmt, uint16_t nchan, uint32_t rate)
{
	fmt->audio_format = WAVE_FORMAT_PCM;
	fmt->bits_per_sample = BITS_PER_SAMPLE;
	fmt->nchannels = nchan;
	fmt->sample_rate = rate;
	fmt->block_align = fmt->bits_per_sample * fmt->nchannels / 8;
	fmt->bytes_per_second = fmt->block_align * fmt->sample_rate;
}

static bool setup_reader(struct WavFile *wf)
{
	uint32_t tag, riff_size, chunk_size;
	bool have_fmt = false;

	if (!read_header(wf, &tag, &riff_size))
		return false;
	if (tag != TAG_RIFF || riff_size < 4)
		return false;
	if (!read_u32(wf, &tag) || tag != TAG_WAVE)
		return false;
	uint32_t riff_avail = riff_size - 4;

	while (riff_avail > 8) {
		if (!read_header(wf, &tag, &chunk_size))
			return false;
		riff_avail -= 8;

		if (chunk_size > riff_avail)
			chunk_size = riff_avail;
		riff_avail -= chunk_size;

		if (tag == TAG_FMT) {
			if (!read_format(wf, chunk_size))
				return false;
			have_fmt = true;
		} else if (tag == TAG_DATA) {
			if (!have_fmt)
				return false;
			wf->data_remain = chunk_size;
			return true;
		} else {
			if (!skip_block(wf, chunk_size))
				return false;
		}
	}
	return false;
}

static bool setup_writer(struct WavFile *wf)
{
	if (!write_header(wf, TAG_RIFF, 0))
		return false;
	wf->riff_size_ofs = ftell(wf->f) - 4;
	if (!write_u32(wf, TAG_WAVE))
		return false;
	if (!write_format(wf))
		return false;
	if (!write_header(wf, TAG_DATA, 0))
		return false;
	wf->data_size_ofs = ftell(wf->f) - 4;
	return true;
}

/*
 * Public API
 */

struct WavFile *wav_open_reader(const char *fn)
{
	struct WavFile *wf = calloc(1, sizeof(struct WavFile));
	if (!wf)
		return NULL;
	wf->f = fopen(fn, "rb");
	if (!wf->f) {
		free(wf);
		return NULL;
	}
	if (!setup_reader(wf)) {
		wav_close(wf);
		return NULL;
	}
	return wf;
}

struct WavFile *wav_open_writer(const char *fn, uint16_t nchan, uint32_t rate)
{
	struct WavFile *wf = calloc(1, sizeof(struct WavFile));
	if (!wf)
		return NULL;

	wf->f = fopen(fn, "wb");
	if (!wf->f) {
		free(wf);
		return NULL;
	}

	fill_format(&wf->fmt, nchan, rate);

	if (!setup_writer(wf)) {
		wav_close(wf);
		return NULL;
	}
	return wf;
}

uint16_t wav_nchannels(struct WavFile *wf)
{
	return wf->fmt.nchannels;
}

uint32_t wav_sample_rate(struct WavFile *wf)
{
	return wf->fmt.sample_rate;
}

size_t wav_read_data(struct WavFile *wf, void *data, size_t nbytes)
{
	size_t size = nbytes < wf->data_remain ? nbytes : wf->data_remain;
	size_t res = fread(data, 1, size, wf->f);
	wf->data_remain -= res;
	return res;
}

bool wav_read_sample(struct WavFile *wf, int16_t *sample)
{
	uint8_t buf[2];
	if (wav_read_data(wf, buf, 2) != 2)
		return false;
	*sample = (int16_t)U16(buf[0], buf[1]);
	return true;
}

bool wav_write_data(struct WavFile *wf, const void *data, size_t nbytes)
{
	return fwrite(data, 1, nbytes, wf->f) == nbytes;
}

bool wav_write_sample(struct WavFile *wf, int16_t sample)
{
	return write_u16(wf, sample);
}

bool wav_write_finish(struct WavFile *wf)
{
	uint32_t end_ofs = ftell(wf->f);
	uint32_t data_size = end_ofs - wf->data_size_ofs - 4;
	uint32_t riff_size = end_ofs - wf->riff_size_ofs - 4;
	if (!write_final_size(wf, wf->riff_size_ofs, riff_size))
		return false;
	if (!write_final_size(wf, wf->data_size_ofs, data_size))
		return false;
	if (fseek(wf->f, end_ofs, SEEK_SET) != 0)
		return false;
	return true;
}

void wav_close(struct WavFile *wf)
{
	if (wf->f)
		fclose(wf->f);
	free(wf);
}
