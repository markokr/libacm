#ifndef _WAVIO_H_
#define _WAVIO_H_

#include <stdint.h>
#include <stdbool.h>

struct WavFile;

struct WavFile *wav_open_reader(const char *fn);
struct WavFile *wav_open_writer(const char *fn, uint16_t nchan, uint32_t rate);

uint16_t wav_nchannels(struct WavFile *wf);
uint32_t wav_sample_rate(struct WavFile *wf);

size_t wav_read_data(struct WavFile *wf, void *data, size_t nbytes);
bool wav_read_sample(struct WavFile *wf, int16_t *sample);

bool wav_write_data(struct WavFile *wf, const void *data, size_t nbytes);
bool wav_write_sample(struct WavFile *wf, int16_t sample);

bool wav_write_finish(struct WavFile *wf);

void wav_close(struct WavFile *wf);

#endif
