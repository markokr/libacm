#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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

static int xread(struct WavFile *wf, void *data, size_t size)
{
	int res = fread(data, size, 1, wf->f);
	return res != 1 ? -1 : 0;
}

static int xwrite(struct WavFile *wf, const void *data, size_t size)
{
	int res = fwrite(data, size, 1, wf->f);
	return res != 1 ? -1 : 0;
}

static int read_u16(struct WavFile *wf, uint16_t *out)
{
	uint8_t buf[2];
	int err = xread(wf, buf, 2);
	if (err == 0) {
		*out = U16(buf[0], buf[1]);
	}
	return err;
}

static int read_u32(struct WavFile *wf, uint32_t *out)
{
	uint8_t buf[4];
	int err = xread(wf, buf, 4);
	if (err == 0) {
		*out = U32(buf[0], buf[1], buf[2], buf[3]);
	}
	return err;
}

static int read_header(struct WavFile *wf, uint32_t *tag, uint32_t *size)
{
	int err = read_u32(wf, tag);
	if (err == 0) {
		err = read_u32(wf, size);
	}
	return err;
}

static int skip_block(struct WavFile *wf, size_t n)
{
	if (n == 0)
		return 0;
	return fseek(wf->f, n + (n & 1), SEEK_CUR);
}

static int write_u16(struct WavFile *wf, uint16_t val)
{
	uint8_t buf[2] = { val & 255, val >> 8 };
	return xwrite(wf, buf, 2);
}

static int write_u32(struct WavFile *wf, uint32_t val)
{
	uint8_t buf[4] = { val & 255, (val >> 8) & 255, (val >> 16) & 255, (val >> 24) & 255 };
	return xwrite(wf, buf, 4);
}

static int write_header(struct WavFile *wf, uint32_t tag, uint32_t size)
{
	int err = write_u32(wf, tag);
	if (err == 0) {
		err = write_u32(wf, size);
	}
	return err;
}

static int write_final_size(struct WavFile *wf, uint32_t ofs, uint32_t size)
{
	if (fseek(wf->f, ofs, SEEK_SET) != 0)
		return -1;
	return write_u32(wf, size);
}

static int read_format(struct WavFile *wf, uint32_t size)
{
	struct Format *fmt = &wf->fmt;
	int err;

	if (size < FMT_SIZE)
		goto invalid;

	err = read_u16(wf, &fmt->audio_format);
	if (err == 0)
		err = read_u16(wf, &fmt->nchannels);
	if (err == 0)
		err = read_u32(wf, &fmt->sample_rate);
	if (err == 0)
		err = read_u32(wf, &fmt->bytes_per_second);
	if (err == 0)
		err = read_u16(wf, &fmt->block_align);
	if (err == 0)
		err = read_u16(wf, &fmt->bits_per_sample);
	if (err == 0)
		err = skip_block(wf, size - FMT_SIZE);
	if (err)
		return err;

	if (fmt->audio_format != WAVE_FORMAT_PCM || fmt->bits_per_sample != BITS_PER_SAMPLE)
		goto invalid;
	if (fmt->nchannels == 0 || fmt->sample_rate == 0)
		goto invalid;

	return 0;
invalid:
	errno = EINVAL;
	return -1;
}

static int write_format(struct WavFile *wf)
{
	int err = write_header(wf, TAG_FMT, FMT_SIZE);
	if (err == 0)
		err = write_u16(wf, wf->fmt.audio_format);
	if (err == 0)
		err = write_u16(wf, wf->fmt.nchannels);
	if (err == 0)
		err = write_u32(wf, wf->fmt.sample_rate);
	if (err == 0)
		err = write_u32(wf, wf->fmt.bytes_per_second);
	if (err == 0)
		err = write_u16(wf, wf->fmt.block_align);
	if (err == 0)
		err = write_u16(wf, wf->fmt.bits_per_sample);
	return err;
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

static int setup_reader(struct WavFile *wf)
{
	uint32_t tag, riff_size, chunk_size;
	int have_fmt = 0;

	int err = read_header(wf, &tag, &riff_size);
	if (err)
		return err;
	if (tag != TAG_RIFF || riff_size < 4)
		goto invalid;

	err = read_u32(wf, &tag);
	if (err)
		return err;
	if (tag != TAG_WAVE)
		goto invalid;

	uint32_t riff_avail = riff_size - 4;
	while (riff_avail > 8) {
		err = read_header(wf, &tag, &chunk_size);
		if (err)
			return err;
		riff_avail -= 8;

		if (chunk_size > riff_avail)
			chunk_size = riff_avail;
		riff_avail -= chunk_size;

		if (tag == TAG_FMT) {
			err = read_format(wf, chunk_size);
			if (err)
				return err;
			have_fmt = 1;
		} else if (tag == TAG_DATA) {
			if (!have_fmt)
				goto invalid;
			wf->data_remain = chunk_size;
			return 0;
		} else {
			err = skip_block(wf, chunk_size);
			if (err)
				return err;
		}
	}
invalid:
	errno = EINVAL;
	return -1;
}

static int setup_writer(struct WavFile *wf)
{
	long ofs;
	int err = write_header(wf, TAG_RIFF, 0);
	if (err == 0) {
		ofs = ftell(wf->f);
		if (ofs < 0)
			return -1;
		wf->riff_size_ofs = ofs - 4;
	}
	if (err == 0)
		err = write_u32(wf, TAG_WAVE);
	if (err == 0)
		err = write_format(wf);
	if (err == 0)
		err = write_header(wf, TAG_DATA, 0);
	if (err == 0) {
		ofs = ftell(wf->f);
		if (ofs < 0)
			return -1;
		wf->data_size_ofs = ofs - 4;
	}
	return err;
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
	int err = setup_reader(wf);
	if (err) {
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

	int err = setup_writer(wf);
	if (err) {
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

ssize_t wav_read_data(struct WavFile *wf, void *data, size_t nbytes)
{
	size_t size = nbytes < wf->data_remain ? nbytes : wf->data_remain;
	if (size == 0) {
		return 0;
	}
	int err = xread(wf, data, size);
	if (err == 0) {
		wf->data_remain -= size;
		return size;
	}
	return err;
}

int wav_read_sample(struct WavFile *wf, int16_t *sample)
{
	uint8_t buf[2];
	ssize_t res = wav_read_data(wf, buf, 2);
	if (res > 0) {
		*sample = (int16_t)U16(buf[0], buf[1]);
		return 0;
	}
	return -1;
}

int wav_write_data(struct WavFile *wf, const void *data, size_t nbytes)
{
	return xwrite(wf, data, nbytes);
}

int wav_write_sample(struct WavFile *wf, int16_t sample)
{
	return write_u16(wf, sample);
}

int wav_write_finish(struct WavFile *wf)
{
	uint32_t end_ofs = ftell(wf->f);
	uint32_t data_size = end_ofs - wf->data_size_ofs - 4;
	uint32_t riff_size = end_ofs - wf->riff_size_ofs - 4;

	int err = write_final_size(wf, wf->riff_size_ofs, riff_size);
	if (err == 0) {
		err = write_final_size(wf, wf->data_size_ofs, data_size);
	}
	if (err == 0) {
		err = fseek(wf->f, end_ofs, SEEK_SET);
	}
	return err;
}

void wav_close(struct WavFile *wf)
{
	if (wf->f)
		fclose(wf->f);
	free(wf);
}
