#ifndef _WAVIO_H_
#define _WAVIO_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

struct WavFile {
	FILE *f;
	uint16_t channels;
	uint32_t sample_rate;

	/* used when reading */
	uint32_t data_size;
	uint32_t read_ofs;
	uint32_t end_ofs;

	/* used when reading & writing */
	uint32_t riff_size_ofs;
	uint32_t data_size_ofs;
};

struct WavFile *wav_open_reader(const char *fn);
struct WavFile *wav_open_writer(const char *fn, uint16_t nchan, uint32_t rate);

bool wav_read_sample(struct WavFile *wav, int16_t *sample);

bool wav_write_data(struct WavFile *wav, const void *data, size_t nbytes);
bool wav_write_finish(struct WavFile *wav);

void wav_close(struct WavFile *wav);

#endif
