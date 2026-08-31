/*
 * Command line tool for ACM manipulating.
 *
 * Copyright (c) 2004-2010, Marko Kreen
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

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include "libacm.h"
#include "encode.h"
#include "wavio.h"

static const char *version = "acmtool - libacm version " LIBACM_VERSION;

static int cf_force_chans = 0;
static int cf_no_output = 0;
static int cf_quiet = 0;
static int cf_wavc = 0;

static void show_header(const char *fn, ACMStream *acm)
{
	int kbps;
	const ACMInfo *inf;
	unsigned m, s, tmp;
	if (cf_quiet)
		return;
	inf = acm_info(acm);
	kbps = acm_bitrate(acm) / 1000;
	tmp = acm_time_total(acm) / 1000;
	s = tmp % 60;
	m = tmp / 60;
	printf("%s: Length:%2d:%02d Chans:%d(%d) Freq:%d A:%d/%d kbps:%d\n", fn, m, s,
	       acm_channels(acm), acm->info.acm_channels, acm_rate(acm), inf->acm_level,
	       inf->acm_rows, kbps);
}

static void *xmalloc(size_t len)
{
	void *p = malloc(len);
	if (!p) {
		perror("malloc");
		exit(1);
	}
	return p;
}

#ifdef HAVE_AO

/*
 * Audio playback with libao
 */

#include <ao/ao.h>

static ao_device *dev = NULL;
static ao_sample_format old_fmt;

static ao_device *open_audio(ao_sample_format *fmt)
{
	if (dev && memcmp(fmt, &old_fmt, sizeof(old_fmt)) != 0) {
		ao_close(dev);
		dev = NULL;
	}
	if (dev == NULL) {
		int id = ao_default_driver_id();
		if (id < 0) {
			fprintf(stderr, "failed to find audio driver\n");
			exit(1);
		}
		dev = ao_open_live(id, fmt, NULL);
		old_fmt = *fmt;
	}
	if (dev == NULL) {
		fprintf(stderr, "failed to open audio device\n");
		exit(1);
	}
	return dev;
}

static void close_audio(void)
{
	if (dev)
		ao_close(dev);
	dev = NULL;
}

static void play_file(const char *fn)
{
	ACMStream *acm;
	int err, res, buflen;
	ao_sample_format fmt;
	ao_device *dev;
	char *buf;
	unsigned int total_bytes, bytes_done = 0;

	err = acm_open_file(&acm, fn, cf_force_chans);
	if (err < 0) {
		fprintf(stderr, "%s: %s\n", fn, acm_strerror(err));
		return;
	}
	show_header(fn, acm);

	memset(&fmt, 0, sizeof fmt);
	fmt.bits = 16;
	fmt.rate = acm_rate(acm);
	fmt.channels = acm_channels(acm);
	fmt.byte_format = AO_FMT_LITTLE;

	dev = open_audio(&fmt);

	buflen = 4 * 1024;
	buf = xmalloc(buflen);

	total_bytes = acm_pcm_total(acm) * acm_channels(acm) * ACM_WORD;
	while (bytes_done < total_bytes) {
		res = acm_read_loop(acm, buf, buflen, 0, 2, 1);
		if (res == 0)
			break;
		if (res > 0) {
			bytes_done += res;
			res = ao_play(dev, buf, res);
		} else {
			fprintf(stderr, "%s: %s\n", fn, acm_strerror(res));
			break;
		}
	}

	memset(buf, 0, buflen);
	if (bytes_done < total_bytes)
		fprintf(stderr, "%s: adding filler_samples: %d\n", fn, total_bytes - bytes_done);
	while (bytes_done < total_bytes) {
		int bs;
		if (bytes_done + buflen > total_bytes) {
			bs = total_bytes - bytes_done;
		} else {
			bs = buflen;
		}
		res = ao_play(dev, buf, bs);
		if (res != bs)
			break;
		bytes_done += res;
	}

	acm_close(acm);
	free(buf);
}

#endif /* HAVE_AO */

/*
 * WAV writing
 */

static char *makefn(const char *fn, const char *ext)
{
	char *dstfn, *p, *slash;
	dstfn = xmalloc(strlen(fn) + strlen(ext) + 2);
	strcpy(dstfn, fn);
	slash = strrchr(dstfn, '/');
	p = strrchr(slash ? slash : dstfn, '.');
	if (p != NULL)
		*p = 0;
	strcat(dstfn, ext);
	return dstfn;
}

static bool output_data(struct WavFile *wav, const void *data, size_t size)
{
	if (cf_no_output)
		return true;
	return wav_write_data(wav, data, size);
}

static void decode_file(const char *fn, const char *fn2)
{
	ACMStream *acm = NULL;
	char *buf;
	int res, buflen, err;
	struct WavFile *wav = NULL;
	unsigned int bytes_done = 0, total_bytes;

	err = acm_open_file(&acm, fn, cf_force_chans);
	if (err < 0) {
		fprintf(stderr, "%s: %s\n", fn, acm_strerror(err));
		exit(1);
	}

	if (!cf_no_output) {
		wav = wav_open_writer(fn2, acm->info.channels, acm->info.rate);
		if (wav == NULL)
			goto write_error;
	}

	show_header(fn, acm);

	buflen = 16 * 1024;
	buf = xmalloc(buflen);

	total_bytes = acm_pcm_total(acm) * acm_channels(acm) * ACM_WORD;
	while (bytes_done < total_bytes) {
		res = acm_read_loop(acm, buf, buflen, 0, 2, 1);
		if (res == 0)
			break;
		if (res > 0) {
			if (!output_data(wav, buf, res))
				goto write_error;
			bytes_done += res;
		} else {
			goto read_error;
		}
	}

	memset(buf, 0, buflen);
	if (bytes_done < total_bytes)
		fprintf(stderr, "%s: adding filler_samples: %d\n", fn, total_bytes - bytes_done);
	while (bytes_done < total_bytes) {
		int bs;
		if (bytes_done + buflen > total_bytes) {
			bs = total_bytes - bytes_done;
		} else {
			bs = buflen;
		}
		if (!output_data(wav, buf, bs)) {
			break;
		}
		bytes_done += bs;
	}
	acm_close(acm);
	if (wav) {
		if (!wav_write_finish(wav))
			goto write_error;
		wav_close(wav);
	}
	free(buf);
	return;
read_error:
	fprintf(stderr, "%s: %s\n", fn, acm_strerror(res));
	exit(1);
write_error:
	perror(fn2);
	exit(1);
}

static int32_t read_from_wav(void *wav)
{
	int16_t sample;
	if (wav_read_sample(wav, &sample)) {
		return sample;
	}
	return ReadSampleEof;
}

static void encode_file(const char *fn, const char *fn2)
{
	if (!cf_quiet)
		printf("%s -> %s\n", fn, fn2);

	struct WavFile *wav = wav_open_reader(fn);
	if (!wav) {
		perror(fn);
		exit(1);
	}

	FILE *out = fopen(fn2, "wb");
	if (!out) {
		perror(fn2);
		exit(1);
	}

	float factor = wav->sample_rate <= 22050 ? 4.0f : 8.0f;
	float volume = 0.97;
	int levels = 7;
	int samples_per_subband = 2048 / (1 << levels);

	int32_t res = acm_encode(read_from_wav, wav, out, wav->channels, wav->sample_rate, volume,
				 levels, samples_per_subband, 1.0f / factor, cf_wavc);
	if (!res)
		fprintf(stderr, "%s: encoding failed\n", fn);
	fflush(out);
	fclose(out);
	wav_close(wav);
}

/*
 * Modify header
 */

static void set_channels(const char *fn, int n_chan)
{
	FILE *f;
	static const unsigned char acm_id[] = { 0x97, 0x28, 0x03, 0x01 };
	unsigned char hdr[14];
	int oldnum, res;

	if ((f = fopen(fn, "rb+")) == NULL) {
		perror(fn);
		return;
	}
	res = fread(hdr, 1, 14, f);
	if (res != 14) {
		fprintf(stderr, "%s: cannot read header\n", fn);
		goto error;
	}

	if (memcmp(hdr, acm_id, 4)) {
		fprintf(stderr, "%s: not an ACM file\n", fn);
		goto error;
	}

	oldnum = (hdr[9] << 8) + hdr[8];
	if (oldnum != 1 && oldnum != 2) {
		fprintf(stderr, "%s: suspicious number of channels: %d\n", fn, oldnum);
		goto error;
	}

	if (fseek(f, 0, SEEK_SET)) {
		perror(fn);
		goto error;
	}

	hdr[8] = n_chan;
	res = fwrite(hdr, 1, 14, f);
	if (res != 14) {
		perror(fn);
	}
error:
	fclose(f);
}

/*
 * Just show info
 */

static void show_info(const char *fn)
{
	int err;
	ACMStream *acm;

	err = acm_open_file(&acm, fn, cf_force_chans);
	if (err < 0) {
		printf("%s: %s\n", fn, acm_strerror(err));
		return;
	}

	show_header(fn, acm);
	acm_close(acm);
}

static void usage(int err)
{
	printf("%s\n", version);
	printf("Play:   acmtool -p [-q][-m|-s] acmfile [acmfile ...]\n");
	printf("Decode: acmtool -d [-q][-m|-s] -o wavfile acmfile\n");
	printf("        acmtool -d [-q][-m|-s] [-n] acmfile [acmfile ...]\n");
	printf("Encode: acmtool -e [-q][-w] -o acmfile wavfile\n");
	printf("        acmtool -e [-q][-w] wavfile [wavfile ...]\n");
	printf("Other:  acmtool -i acmfile [acmfile ...]\n");
	printf("        acmtool -M|-S acmfile [acmfile ...]\n");
	printf("Commands:\n");
	printf("  -p     play file(s)\n");
	printf("  -d     decode audio into WAV files\n");
	printf("  -e     encode WAV files into ACM\n");
	printf("  -i     show info about ACM files\n");
	printf("  -M     modify ACM header to have 1 channel\n");
	printf("  -S     modify ACM header to have 2 channels\n");
	printf("Switches:\n");
	printf("  -m     force mono\n");
	printf("  -s     force stereo (default)\n");
	printf("  -w     encode to WAVC format\n");
	printf("  -q     be quiet\n");
	printf("  -n     no output - for benchmarking\n");
	printf("  -o FN  output to file, can be used if single source file\n");
	exit(err);
}

typedef void (*ProcessFunc)(const char *fn, const char *fn2);

int main(int argc, char *argv[])
{
	int c, i;
	char *fn, *fn2 = NULL;
	int cmd_decode = 0;
	int cmd_encode = 0;
	int cmd_chg_channels = 0;
	int cmd_info = 0, cmd_play = 0;
	int cf_set_chans = 0;
	ProcessFunc process_func = NULL;
	const char *target_ext = NULL;

	while ((c = getopt(argc, argv, "pdeiMSqhmsnvo:w")) != -1) {
		switch (c) {
		case 'h':
			usage(0);
			break;
		case 'd':
			cmd_decode = 1;
			process_func = decode_file;
			target_ext = ".wav";
			break;
		case 'e':
			cmd_encode = 1;
			process_func = encode_file;
			target_ext = ".acm";
			break;
		case 'i':
			cmd_info = 1;
			break;
		case 'p':
			cmd_play = 1;
			break;
		case 'M':
			cmd_chg_channels = 1;
			cf_set_chans = 1;
			break;
		case 'S':
			cmd_chg_channels = 1;
			cf_set_chans = 2;
			break;
		case 'q':
			cf_quiet = 1;
			break;
		case 'm':
			cf_force_chans = 1;
			break;
		case 's':
			cf_force_chans = 2;
			break;
		case 'w':
			cf_wavc = 1;
			break;
		case 'n':
			cf_no_output = 1;
			break;
		case 'o':
			fn2 = optarg;
			break;
		case 'v':
			printf("%s\n", version);
			exit(0);
		default:
			fprintf(stderr, "bad arg: -%c\n", c);
			usage(1);
		}
	}
	i = cmd_chg_channels + cmd_info + cmd_decode + cmd_play + cmd_encode;
	if (i < 1) {
		fprintf(stderr, "need command, use -h for help\n");
		exit(1);
	}
	if (i > 1) {
		fprintf(stderr, "only one command at a time please\n");
		exit(1);
	}

	/* play file */
	if (cmd_play) {
#ifdef HAVE_AO
		ao_initialize();
		for (i = optind; i < argc; i++)
			play_file(argv[i]);
		close_audio();
		ao_shutdown();
		return 0;
#else
		fprintf(stderr, "For audio output, please compile with libao.\n");
		return 1;
#endif
	}

	/* show info */
	if (cmd_info) {
		for (i = optind; i < argc; i++)
			show_info(argv[i]);
		return 0;
	}

	/* channel changing */
	if (cmd_chg_channels) {
		for (i = optind; i < argc; i++)
			set_channels(argv[i], cf_set_chans);
		return 0;
	}

	/* regular converting */
	if (optind == argc || !(cmd_encode || cmd_decode))
		usage(1);
	if (fn2) {
		if (optind + 1 != argc)
			usage(1);
		fn = argv[optind];
		process_func(fn, fn2);
	} else {
		while (optind < argc) {
			fn = argv[optind++];
			fn2 = makefn(fn, target_ext);
			process_func(fn, fn2);
			free(fn2);
		}
	}
	return 0;
}
