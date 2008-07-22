/*
 * Command line tool for ACM manipulating.
 *
 * Copyright (c) 2004-2008, Marko Kreen
 *
 * Permission to use, copy, modify, and distribute this software for any
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

static const char * version = "acmtool - libacm version " LIBACM_VERSION;

static int cf_raw = 0;
static int cf_force_chans = 0;
static int cf_no_output = 0;
static int cf_quiet = 0;

static void show_header(const char *fn, ACMStream *acm)
{
	int kbps;
	const ACMInfo *inf;
	if (cf_quiet)
		return;
	inf = acm_info(acm);
	kbps = acm_bitrate(acm) / 1000;
	printf("%s: Samples:%d Chans:%d Freq:%d A1:0x%02x A2:0x%04x kbps:%d\n",
			fn, acm_pcm_total(acm), acm_channels(acm), acm_rate(acm),
			inf->acm_level, inf->acm_rows, kbps);
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
	if (dev && memcmp(fmt, &old_fmt, sizeof(old_fmt))) {
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
		printf("%s: %s\n", fn, acm_strerror(err));
		return;
	}
	show_header(fn, acm);
	fmt.bits = 16;
	fmt.rate = acm_rate(acm);
	fmt.channels = acm_channels(acm);
	fmt.byte_format = AO_FMT_LITTLE;

	dev = open_audio(&fmt);

	buflen = 4*1024;
	buf = malloc(buflen);

	total_bytes = acm_pcm_total(acm) * acm_channels(acm) * ACM_WORD;
	while (bytes_done < total_bytes) {
		res = acm_read_loop(acm, buf, buflen/ACM_WORD, 0,2,1);
		if (res == 0)
			break;
		if (res > 0) {
			bytes_done += res;
			res = ao_play(dev, buf, res);
		} else {
			if (!cf_quiet)
				printf("%s: %s\n", fn, acm_strerror(res));
			break;
		}
	}

	memset(buf, 0, buflen);
	if (bytes_done < total_bytes && !cf_quiet) 
		fprintf(stderr, "adding filler_samples: %d\n",
				total_bytes - bytes_done);
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

static char * makefn(const char *fn, const char *ext)
{
	char *dstfn, *p;
	dstfn = malloc(strlen(fn) + strlen(ext) + 2);
	strcpy(dstfn, fn);
	p = strrchr(dstfn, '.');
	if (p != NULL)
		*p = 0;
	strcat(dstfn, ext);
	return dstfn;
}

#define put_word(p, val) do { \
		*p++ = val & 0xFF; \
		*p++ = (val >> 8) & 0xFF; \
	} while (0)

#define put_dword(p, val) do { \
		*p++ = val & 0xFF; \
		*p++ = (val >> 8) & 0xFF; \
		*p++ = (val >> 16) & 0xFF; \
		*p++ = (val >> 24) & 0xFF; \
	} while (0)

#define put_data(p, data, len) do { \
		memcpy(p, data, len); \
		p += len; \
	} while (0)

static int write_wav_header(FILE *f, ACMStream *acm)
{
	unsigned char hdr[50], *p = hdr;
	int res;
	unsigned datalen = acm_pcm_total(acm) * ACM_WORD * acm_channels(acm);
	
	int code = 1;
	unsigned n_channels = acm_channels(acm);
	unsigned srate = acm_rate(acm);
	unsigned avg_bps = srate * n_channels * ACM_WORD;
	unsigned significant_bits = ACM_WORD * 8;
	unsigned block_align = significant_bits * n_channels / 8;
	unsigned hdrlen = 16;
	unsigned wavlen = 4 + 8 + hdrlen + 8 + datalen;
	
	memset(hdr, 0, sizeof(hdr));
	
	put_data(p, "RIFF", 4);
	put_dword(p, wavlen);
	put_data(p, "WAVEfmt ", 8);
	put_dword(p, hdrlen);
	put_word(p, code);
	put_word(p, n_channels);
	put_dword(p, srate);
	put_dword(p, avg_bps);
	put_word(p, block_align);
	put_word(p, significant_bits);
	
	put_data(p, "data", 4);
	put_dword(p, datalen);

	res = fwrite(hdr, 1, p - hdr, f);
	if (res != p - hdr)
		return -1;
	else
		return 0;
}

static void decode_file(const char *fn, const char *fn2)
{
	ACMStream *acm;
	char *buf;
	int res, res2, buflen, err;
	FILE *fo = NULL;
	int bytes_done = 0, total_bytes;

	err = acm_open_file(&acm, fn, cf_force_chans);
	if (err < 0) {
		printf("%s: %s\n", fn, acm_strerror(err));
		return;
	}
	show_header(fn, acm);

	if (!cf_no_output) {
		if (!strcmp(fn2, "-"))
			fo = stdout;
		else
			fo = fopen(fn2, "wb");
		if (fo == NULL) {
			perror(fn2);
			acm_close(acm);
			return;
		}
	}

	if ((!cf_raw) && (!cf_no_output)) {
		if ((err = write_wav_header(fo, acm)) < 0) {
			perror(fn2);
			fclose(fo);
			acm_close(acm);
			return;
		}
	}
	buflen = 16*1024;
	buf = malloc(buflen);

	total_bytes = acm_pcm_total(acm) * acm_channels(acm) * ACM_WORD;
	
	while (bytes_done < total_bytes) {
		res = acm_read_loop(acm, buf, buflen/2, 0,2,1);
		if (res == 0)
			break;
		if (res > 0) {
			if (!cf_no_output) {
				res2 = fwrite(buf, 1, res, fo);
				if (res2 != res) {
					printf("write error\n");
					break;
				}
			}
			bytes_done += res;
		} else {
			if (!cf_quiet)
				printf("%s: %s\n", fn, acm_strerror(res));
			break;
		}
	}

	memset(buf, 0, buflen);
	if (bytes_done < total_bytes && !cf_quiet) 
		fprintf(stderr, "adding filler_samples: %d\n",
				total_bytes - bytes_done);
	while (bytes_done < total_bytes) {
		int bs;
		if (bytes_done + buflen > total_bytes) {
			bs = total_bytes - bytes_done;
		} else {
			bs = buflen;
		}
		if (!cf_no_output) {
			res2 = fwrite(buf, 1, bs, fo);
			if (res2 != bs)
				break;
		}
		bytes_done += bs;
	}

	acm_close(acm);
	if (!cf_no_output)
		fclose(fo);
	free(buf);
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
		return;
	}

	if (memcmp(hdr, acm_id, 4)) {
		fprintf(stderr, "%s: not an ACM file\n", fn);
		return;
	}

	oldnum = (hdr[9] << 8) + hdr[8];
	if (oldnum != 1 && oldnum != 2) {
		fprintf(stderr, "%s: suspicios number of channels: %d\n",
				fn, oldnum);
		return;
	}

	if (fseek(f, 0, SEEK_SET)) {
		perror(fn);
		return;
	}

	hdr[8] = n_chan;
	res = fwrite(hdr, 1, 14, f);
	if (res != 14) {
		perror(fn);
	}
	fclose(f);
}

/*
 * Just show info
 */

static void show_info(const char *fn)
{
	int err;
	ACMStream *acm;

	err = acm_open_file(&acm, fn, 0);
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
	printf("Play:   acmtool -p [-q][-m|-s] infile [infile ...]\n");
	printf("Decode: acmtool -d [-q][-m|-s] [-r|-n] -o outfile infile\n");
	printf("        acmtool -d [-q][-m|-s] [-r|-n] infile [infile ...]\n");
	printf("Other:  acmtool -i ACMFILE [ACMFILE ...]\n");
	printf("        acmtool -M|-S ACMFILE [ACMFILE ...]\n");
	printf("Commands:\n");
	printf("  -d     decode audio into WAV files\n");
	printf("  -i     show info about ACM files\n");
	printf("  -M     modify ACM header to have 1 channel\n");
	printf("  -S     modify ACM header to have 2 channels\n");
	printf("Switches:\n");
	printf("  -m     force mono wav\n");
	printf("  -s     force stereo wav\n");
	printf("  -r     raw output - no wav header\n");
	printf("  -q     be quiet\n");
	printf("  -n     no output - for benchmarking\n");
	printf("  -o FN  output to file, can be used if single source file\n");
	exit(err);
}

int main(int argc, char *argv[])
{
	int c, i;
	char *fn, *fn2 = NULL;
	int cmd_decode = 0;
	int cmd_chg_channels = 0;
	int cmd_info = 0, cmd_play = 0;
	int cf_set_chans = 0;

	while ((c = getopt(argc, argv, "pdiMSqhrmsnvo:")) != -1) {
		switch (c) {
		case 'h':
			usage(0);
			break;
		case 'd':
			cmd_decode = 1;
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
		case 'r':
			cf_raw = 1;
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
			printf("bad arg: -%c\n", c);
			usage(1);
		}
	}
	i = cmd_chg_channels + cmd_info + cmd_decode + cmd_play;
	if (i < 1 || i > 1) {
		fprintf(stderr, "only one command at a time please\n");
		usage(1);
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
	if (optind == argc)
		usage(1);
	if (fn2) {
		if (optind + 1 != argc)
			usage(1);
		fn = argv[optind];
		decode_file(fn, fn2);
	} else {
		while (optind < argc) {
			fn = argv[optind++];
			fn2 = makefn(fn, cf_raw ? ".raw" : ".wav");
			decode_file(fn, fn2);
			free(fn2);
		}
	}
	return 0;
}

