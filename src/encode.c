/*
 * Descent 3
 * Copyright (C) 2024 Parallax Software
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Interplay ACM audio encoder.
 *
 * ACM is a subband / wavelet transform coder.  Encoding one block is three
 * stages, each the inverse of a decoder (decode.c) stage:
 *
 *   1. Analysis filter bank  -- transform() / transform_subband().
 *      A dyadic tree of 2-channel QMF stages recursively splits the signal
 *      into n_columns == 2^levels uniform subbands.  std_lo_filter is the
 *      low-pass (scaling) filter, std_hi_filter the high-pass (wavelet) filter.
 *      This is the inverse of the decoder's juggle_block().
 *
 *   2. Uniform scalar quantization with rate control
 *      -- estimate_bits() / choose_quant_step().
 *      Every subband coefficient is quantized to an integer index
 *      q = floor((x + step/2) / step).  choose_quant_step() binary-searches
 *      the step so the encoded block size meets a target bit budget derived
 *      from the requested compression ratio.
 *
 *   3. Entropy coding -- the pack_*() functions.
 *      Each subband's indices are packed with whichever variable-length code
 *      is cheapest: fixed-width "linear" (pack_linear), run-length "k" codes
 *      (pack_k13 ... pack_k44), or base-3/5/11 grouped "t" codes (pack_t15,
 *      pack_t27, pack_t37).  Each pack_*() is the bit-exact inverse of the
 *      matching decoder filler f_*().
 */

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libacm.h"
#include "encode.h"

#define SAMPLE_WIDTH 16

#define MIN_LEVELS 1
#define MAX_LEVELS 15
#define MAX_COLUMNS (1 << MAX_LEVELS)
#define MIN_ROWS 1
#define MAX_ROWS ((1 << 12) - 1)
#define MAX_BLOCK_LEN (1024 * 1024)

#define VALID(levels, rows) \
	((levels) >= MIN_LEVELS && (levels) <= MAX_LEVELS && (rows) >= MIN_ROWS \
	 && (rows) <= MAX_ROWS && (1 << (levels)) * (rows) <= (MAX_BLOCK_LEN))

// A:8/16 -> 4096
// A:7/16 -> 2048
// A:6/8 -> 512
#define _REASONABLE(levels, rows, cols) \
	((levels) >= 4 && (levels) <= 10 && (rows) >= 8 && (rows) * (cols) >= 256 \
	 && (rows) * (cols) <= 8192)

#define REASONABLE(levels, rows) _REASONABLE(levels, rows, 1 << (levels))

int acm_debug_encoder;

/*
 * Bitstream writer
 */

typedef struct {
	FILE *out;
	uint32_t buf;
	uint32_t count;
} BitsEncoder;

static int bits_write(BitsEncoder *bits, int32_t val, uint32_t n_bits)
{
	assert((n_bits + bits->count) <= 32);
	bits->buf |= (uint32_t)(val) << bits->count;
	bits->count += n_bits;

	while (bits->count >= 8) {
		uint8_t v = bits->buf & 0xFF;
		if (fputc(v, bits->out) < 0)
			return ACM_ERR_WRITE_ERR;
		bits->buf >>= 8;
		bits->count -= 8;
	}
	return 0;
}

static int bits_flush(BitsEncoder *bits)
{
	while (bits->count >= 8) {
		uint8_t v = bits->buf & 0xFF;
		if (fputc(v, bits->out) < 0)
			return ACM_ERR_WRITE_ERR;

		bits->buf >>= 8;
		bits->count -= 8;
	}

	if (bits->count > 0) {
		uint8_t v = bits->buf & 0xFF;
		if (fputc(v, bits->out) < 0)
			return ACM_ERR_WRITE_ERR;
		bits->count = 0;
		bits->buf = 0;
	}
	return 0;
}

static void bits_init(BitsEncoder *bits, FILE *out)
{
	bits->out = out;
	bits->buf = 0;
	bits->count = 0;
}

static inline uint32_t fourcc(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	return ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16)
		| ((uint32_t)(d) << 24));
}

/*
 * Quantiziser for specific step size
 */

typedef struct {
	float step;
	float half_step;
	int32_t min_word;
	int32_t max_word;
} Quantizer;

static void quant_init(Quantizer *q, int step)
{
	q->step = (float)step;
	q->half_step = q->step * 0.5f;
	q->min_word = (int32_t)ceilf(-32767.0f / q->step);
	q->max_word = (int32_t)floorf(32767.0f / q->step);
}

static int32_t quant_value(Quantizer *q, float value)
{
	int32_t w = (int32_t)floorf((value + q->half_step) / q->step);
	if (w < q->min_word) {
		w = q->min_word;
	} else if (w > q->max_word) {
		w = q->max_word;
	}
	return w;
}

typedef struct Filter {
	int filter_len;	      /* odd number */
	const float *lo, *hi; /* each (filter_len+1)/2 values */
} Filter;

static const float std_lo_filter[] = {
	-0.0012475221f, -0.0024950907f, 0.0087309526f, 0.019957958f,
	-0.050528999f,	-0.12055097f,	0.29304558f,   0.70617616f,
};

static const float std_hi_filter[] = {
	0.0012475221f, -0.0024950907f, -0.0087309526f, 0.019957958f,
	0.050528999f,  -0.12055097f,   -0.29304558f,   0.70617616f,
};

static const struct Filter std_filter = {
	.filter_len = 15,
	.lo = &std_lo_filter[0],
	.hi = &std_hi_filter[0],
};

/*
 * Main encoder state
 */

#define MAX_SLOTS (MAX_LEVELS + 1)

typedef enum PackerId {
	ZeroFill,
	Binary3 = 3,
	Binary16 = 16,

	Peak1ZZ,
	Peak1Z,
	Peak1Base3,

	Peak2ZZ,
	Peak2Z,
	Peak2Base5,

	Peak3ZZ,
	Peak3Z,
	_Unused_25_,

	Peak4ZZ,
	Peak4Z,
	_Unused_28_,

	Peak5Base11,
} PackerId;

typedef struct {
	ReadSampleFunction *reader_func;
	void *reader_arg;
	int reader_eof;

	BitsEncoder bits;
	long acm_start_ofs;
	long wavc_start_ofs;
	int wavc;

	uint32_t sample_count; /* nubmer of samples accesped */
	int enable_output;     /* disable output while priming */

	const Filter *filter; /* tranform filter */
	float volume;	      /* scale input samples */
	int bit_budget;	      /* output bits per block */

	int8_t n_levels; /* decomposition depth */
	int n_columns;	 /* subband count (1 << levels) */
	int n_rows;	 /* samples per subband */

	int block_len;	 /* n_columns * n_rows */
	int priming_len; /* filter warm-up samples fed before/after the signal */

	float *level_slots[MAX_SLOTS]; /* per-level blocks */
	int input_pos;		       /* pos in level_slots[0] for new samples */

	uint8_t *column_format; /* chosen packer for subband */
	int quant_power;	/* log2 of the dequant table size */
	int quant_step;		/* uniform quantizer step size  */
	Quantizer quantizer;	/* convert coeffs to code words */
} Encoder;

#define OUTPUT_BITS(enc, value, nbits) \
	do { \
		int err = bits_write(&((enc)->bits), value, nbits); \
		if (err) \
			return err; \
	} while (0)

/*
 * Subband encoders
 */

/* w={-peak .. +peak}, w!=0, res={0 .. (peak*2-1)} */
#define SHIFT_NZ(w, peak) ((w) + (peak) + ((w) < 0 ? 0 : -1))

/* w={-3,-2,+2,+3}, res={0 .. 3} */
#define SHIFT_2_3(w) ((w) < 0 ? (w) + 3 : (w))

typedef int (*PackFunc)(Encoder *enc, int col, PackerId formatId);

static int32_t codeword(Encoder *enc, int row, int col)
{
	float *values = enc->level_slots[enc->n_levels];
	float value = values[(row * enc->n_columns) + col];
	return quant_value(&enc->quantizer, value);
}

static int last_row(Encoder *enc, int row)
{
	return row == enc->n_rows - 1;
}

static int zero_follows(Encoder *enc, int row, int col)
{
	return !last_row(enc, row) && codeword(enc, row + 1, col) == 0;
}

static int pack_zero(Encoder *enc, int col, PackerId fmt)
{
	return 0;
}

static int pack_binary(Encoder *enc, int col, PackerId fmt)
{
	int32_t mid = (1 << (fmt - 1));
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		OUTPUT_BITS(enc, w + mid, fmt);
	}
	return 0;
}

/* Words {-1..1}, assume zero pair */
static int pack_peak1_zz(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (zero_follows(enc, row, col)) {
				/* 0 */
				OUTPUT_BITS(enc, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				OUTPUT_BITS(enc, 1, 2);
			}
		} else {
			/* 1, 1, ? */
			OUTPUT_BITS(enc, 3, 2);
			OUTPUT_BITS(enc, SHIFT_NZ(w, 1), 1);
		}
	}
	return 0;
}

/* Words {-1..1}, assume zero */
static int pack_peak1_z(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			/* 0 */
			OUTPUT_BITS(enc, 0, 1);
		} else {
			/* 1, ? */
			OUTPUT_BITS(enc, 1, 1);
			OUTPUT_BITS(enc, SHIFT_NZ(w, 1), 1);
		}
	}
	return 0;
}

/* 3 words of {-1,0,1} per 5-bits */
static int pack_peak1_base3(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		int32_t packed = w + 1;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 1) * 3;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 1) * 9;

		OUTPUT_BITS(enc, packed, 5);
	}
	return 0;
}

/* Words {-2..2}, assume zero pair */
static int pack_peak2_zz(Encoder *enc, int col, PackerId fmt)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (zero_follows(enc, row, col)) {
				/* 0 */
				OUTPUT_BITS(enc, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				OUTPUT_BITS(enc, 1, 2);
			}
		} else {
			/* 1, 1, ?, ? */
			OUTPUT_BITS(enc, 3, 2);
			OUTPUT_BITS(enc, SHIFT_NZ(w, 2), 2);
		}
	}
	return 0;
}

/* Words {-2..2}, assume zero */
static int pack_peak2_z(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);

		if (w == 0) {
			/* 0 */
			OUTPUT_BITS(enc, 0, 1);
		} else {
			/* 1, ?, ? */
			OUTPUT_BITS(enc, 1, 1);
			OUTPUT_BITS(enc, SHIFT_NZ(w, 2), 2);
		}
	}
	return 0;
}

/* Base-5 packing: 3 words in {-2..2} per 7-bits */
static int pack_peak2_base5(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		int32_t packed = w + 2;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 2) * 5;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 2) * 25;

		OUTPUT_BITS(enc, packed, 7);
	}
	return 0;
}

/* Words {-3..3}, assume zero pair */
static int pack_peak3_zz(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (zero_follows(enc, row, col)) {
				/* 0 */
				OUTPUT_BITS(enc, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				OUTPUT_BITS(enc, 1, 2);
			}
		} else {
			OUTPUT_BITS(enc, 3, 2);
			if (w == -1 || w == 1) {
				/* 1, 1, 0, ? */
				OUTPUT_BITS(enc, 0, 1);
				OUTPUT_BITS(enc, SHIFT_NZ(w, 1), 1);
			} else {
				/* 1, 1, 1, ?, ? */
				OUTPUT_BITS(enc, 1, 1);
				OUTPUT_BITS(enc, SHIFT_2_3(w), 2);
			}
		}
	}
	return 0;
}

/* Words in {-3..3}, assume zero */
static int pack_peak3_z(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			/* 0 */
			OUTPUT_BITS(enc, 0, 1);
		} else {
			OUTPUT_BITS(enc, 1, 1);
			if (w == -1 || w == 1) {
				/* 1, 0, ? */
				OUTPUT_BITS(enc, 0, 1);
				OUTPUT_BITS(enc, SHIFT_NZ(w, 1), 1);
			} else {
				/* 1, 1, ?, ? */
				OUTPUT_BITS(enc, 1, 1);
				OUTPUT_BITS(enc, SHIFT_2_3(w), 2);
			}
		}
	}
	return 0;
}

/* Words in {-4..4}, assume zero pair */
static int pack_peak4_zz(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (zero_follows(enc, row, col)) {
				/* 0 */
				OUTPUT_BITS(enc, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				OUTPUT_BITS(enc, 1, 2);
			}
		} else {
			/* 1, 1, ?, ?, ? */
			OUTPUT_BITS(enc, 3, 2);
			OUTPUT_BITS(enc, SHIFT_NZ(w, 4), 3);
		}
	}
	return 0;
}

/* Words {-4..4}, assume zero */
static int pack_peak4_z(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);

		if (w == 0) {
			/* 0 */
			OUTPUT_BITS(enc, 0, 1);
		} else {
			/* 1, ?, ?, ? */
			OUTPUT_BITS(enc, 1, 1);
			OUTPUT_BITS(enc, SHIFT_NZ(w, 4), 3);
		}
	}
	return 0;
}

/* 2 words in {-5..5} per 7-bits */
static int pack_peak5_base11(Encoder *enc, int col, PackerId fmt)
{
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		int32_t packed = w + 5;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 5) * 11;

		OUTPUT_BITS(enc, packed, 7);
	}
	return 0;
}

/*
 * Packer dispatch table, indexed by the 5-bit format id.  Index-aligned with
 * the decoder's filler_list[] so id N encodes exactly what f_*(N) decodes.
 */
static const PackFunc packer_list[] = {
	/* 0 .. 3 */
	pack_zero, NULL, NULL,

	/* 3 .. 16 */
	pack_binary, pack_binary, pack_binary, pack_binary, pack_binary, pack_binary, pack_binary,
	pack_binary, pack_binary, pack_binary, pack_binary, pack_binary, pack_binary, pack_binary,

	pack_peak1_zz, pack_peak1_z, pack_peak1_base3, // 17 .. 19
	pack_peak2_zz, pack_peak2_z, pack_peak2_base5, // 20 .. 22
	pack_peak3_zz, pack_peak3_z, NULL,	       // 23 .. 25
	pack_peak4_zz, pack_peak4_z, NULL,	       // 26 .. 28
	pack_peak5_base11, NULL, NULL		       // 29 .. 31
};

static const PackerId map_fmt_zz[] = { ZeroFill, Peak1ZZ, Peak2ZZ, Peak3ZZ, Peak4ZZ };
static const PackerId map_fmt_z[] = { ZeroFill, Peak1Z, Peak2Z, Peak3Z, Peak4Z };
static const PackerId map_fmt_flat[] = { ZeroFill, Peak1Base3,	Peak2Base5,
					 Binary3,  Peak5Base11, Peak5Base11 };

static int calc_nbits(int32_t min_word, int32_t max_word)
{
	int32_t mag = (min_word < 0) ? ~min_word : 0;
	if (mag < max_word) {
		mag = max_word;
	}

	int nbits = 1;
	for (; mag != 0 && nbits < 16; nbits++) {
		mag >>= 1;
	}
	return nbits;
}

static int calc_cost_flat(Encoder *enc, int32_t abs_peak, int32_t min_word, int32_t max_word,
			  PackerId *res_col_fmt)
{
	PackerId fmt;
	if (abs_peak <= 5) {
		fmt = map_fmt_flat[abs_peak];
	} else {
		fmt = calc_nbits(min_word, max_word);
	}
	*res_col_fmt = fmt;

	if (fmt == ZeroFill) {
		return 0;
	} else if (fmt == Peak1Base3 || fmt == Peak2Base5) {
		return ((enc->n_rows + 2) / 3) * ((abs_peak * 2) + 3);
	} else if (fmt == Peak5Base11) {
		return ((enc->n_rows + 1) / 2) * 7;
	} else {
		return enc->n_rows * fmt;
	}
}

static int calc_cost_z(Encoder *enc, int32_t abs_peak, int col, PackerId *res_col_fmt)
{
	int cost_zz = 0;
	int cost_z = 0;
	for (int row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (zero_follows(enc, row, col)) {
				cost_zz += 1;
				cost_z += 2;
				row++;
			} else {
				cost_zz += 2;
				cost_z += 1;
			}
		} else if (abs_peak == 1) {
			cost_zz += 3;
			cost_z += 2;
		} else if (abs_peak == 2) {
			cost_zz += 4;
			cost_z += 3;
		} else if (abs_peak == 3 && (w == -1 || w == 1)) {
			cost_zz += 4;
			cost_z += 3;
		} else {
			cost_zz += 5;
			cost_z += 4;
		}
	}

	if (cost_z < cost_zz) {
		*res_col_fmt = map_fmt_z[abs_peak];
		return cost_z;
	} else {
		*res_col_fmt = map_fmt_zz[abs_peak];
		return cost_zz;
	}
}

/*
 * Rate estimation and per-subband format selection for a candidate quant step.
 * Quantizes every coefficient with q = floor((x + step/2) / step) (clamped),
 * finds the peak index per subband, picks the cheapest packer for it, and
 * returns the total encoded size of the block in bits.  Side effects: fills
 * column_format[] and records quant_power / quant_step.
 */
static int estimate_bits(Encoder *enc, int step)
{
	int quant_power = 3;
	int bits = 4 + 16 + enc->n_columns * 5; /* header bits */

	quant_init(&enc->quantizer, step);

	for (int col = 0; col < enc->n_columns; col++) {
		int32_t min_word = 0x10000;
		int32_t max_word = -0x10000;
		for (int row = 0; row < enc->n_rows; row++) {
			int32_t w = codeword(enc, row, col);
			if (w < min_word) {
				min_word = w;
			}
			if (w > max_word) {
				max_word = w;
			}
		}

		int32_t abs_peak = abs(min_word);
		if (abs_peak < max_word) {
			abs_peak = max_word;
		} else if (abs_peak < -max_word) {
			abs_peak = -max_word;
		}

		PackerId fmt;
		int cost = calc_cost_flat(enc, abs_peak, min_word, max_word, &fmt);

		if (abs_peak >= 1 && abs_peak <= 4) {
			PackerId fmt_z;
			int cost_z = calc_cost_z(enc, abs_peak, col, &fmt_z);
			if (cost_z <= cost) {
				cost = cost_z;
				fmt = fmt_z;
			}
		} else if (abs_peak > 5) {
			int nbits = fmt;
			if (quant_power < (nbits - 1)) {
				quant_power = nbits - 1;
			}
		}

		enc->column_format[col] = fmt;
		bits += cost;
	}

	enc->quant_power = quant_power;
	enc->quant_step = step;

	return bits;
}

/*
 * Rate control: binary-search the quant step so the encoded block is the
 * largest that still fits within bit_budget (a smaller step is finer and
 * costs more bits).
 */
static void choose_quant_step(Encoder *enc)
{
	int lo = 1, hi = 0x7FFF;
	do {
		int mid = (lo + hi) / 2;
		int bits = estimate_bits(enc, mid);
		if (bits > enc->bit_budget) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	} while (hi >= lo);

	if (enc->quant_step != lo) {
		estimate_bits(enc, lo);
	}
}

static int write_bands(Encoder *enc)
{
	OUTPUT_BITS(enc, enc->quant_power, 4);
	OUTPUT_BITS(enc, enc->quant_step, 16);
	if (acm_debug_encoder) {
		printf("power=%d step=%d\n", enc->quant_power, enc->quant_step);
	}

	for (int col = 0; col < enc->n_columns; col++) {
		PackerId fmt = enc->column_format[col];
		if (acm_debug_encoder) {
			printf("  %d: %d\n", col, fmt);
		}
		OUTPUT_BITS(enc, fmt, 5);
		if (packer_list[fmt] == NULL)
			return ACM_ERR_CORRUPT;
		int err = packer_list[fmt](enc, col, fmt);
		if (err)
			return err;
	}
	return 0;
}

/*
 * Subband tranform
 */

/*
 * One 2-channel QMF analysis step over a single subband.  Symmetric FIR:
 * even outputs use the low-pass (scaling) filter, odd outputs the high-pass
 * (wavelet) filter, giving a critically-sampled low/high split.  Samples of
 * this subband are interleaved in the buffer with distance `stride`.
 */
static void transform_column(Encoder *enc, const float *src, float *dst, int cols, int rows)
{
	const Filter *filter = enc->filter;
	int halfTaps = (filter->filter_len - 1) / 2; /* taps on each side of center */
	int reach = halfTaps * cols;		     /* offset to the symmetric neighbor */
	src -= reach;

	for (int row = 0; row < rows; row++) {
		const float *coef = (row & 1) ? filter->hi : filter->lo;
		const float *left = src - reach;
		const float *right = src + reach;
		float acc = 0.0f;
		for (int j = halfTaps; j > 0; j--) {
			acc += (*right + *left) * *coef++;
			left += cols;
			right -= cols;
		}
		*dst = (*left * *coef) + acc;

		dst += cols;
		src += cols;
	}
}

/*
 * Forward analysis filter bank: a dyadic tree of 2-channel QMF stages that
 * splits the block into n_columns == 2^levels uniform subbands.  Inverse
 * of the decoder's juggle_block().
 */
static void transform(Encoder *enc)
{
	int cols = 1;
	int rows = enc->block_len; /* samples per subband at this level */
	for (int i = 0; i < enc->n_levels; i++) {
		float *src = enc->level_slots[i];
		float *dst = enc->level_slots[i + 1];

		for (int col = 0; col < cols; col++) {
			transform_column(enc, src + col, dst + col, cols, rows);
		}

		cols *= 2;
		rows /= 2;
	}
}

/*
 * Encoder high-level logic
 */

/*
 * Carry the filter-overlap tail of each level into the next block so the
 * analysis filters stay continuous across block boundaries.
 */
static void shift_overlap(Encoder *enc)
{
	int overlap = enc->filter->filter_len - 1;
	for (int i = 0; i < enc->n_levels; i++, overlap += overlap) {
		float *dst = enc->level_slots[i] - overlap;
		float *src = dst + enc->block_len;
		memcpy(dst, src, overlap * sizeof(float));
	}
}

static int process_block(Encoder *enc)
{
	int err;

	transform(enc);

	if (enc->enable_output) {
		choose_quant_step(enc);

		err = write_bands(enc);
		if (err)
			return err;
	}

	shift_overlap(enc);

	enc->input_pos = 0;
	return 0;
}

static int encode_sample(Encoder *enc)
{
	int16_t sample = 0;

	if (!enc->reader_eof) {
		int err = enc->reader_func(enc->reader_arg, &sample);
		if (err == -1) {
			enc->reader_eof = 1;
			return 0;
		} else if (err != 0) {
			return err;
		}
		enc->sample_count++;
	}

	enc->level_slots[0][enc->input_pos++] = enc->volume * (float)sample;
	if (enc->input_pos == enc->block_len) {
		return process_block(enc);
	}
	return 0;
}

static int encode_flush(Encoder *enc)
{
	// zero-fill partial block
	if (enc->input_pos > 0) {
		for (; enc->input_pos < enc->block_len; enc->input_pos++) {
			enc->level_slots[0][enc->input_pos] = 0.0f;
		}
		int err = process_block(enc);
		if (err)
			return err;
	}

	// NOTE: The Interplay one doesn't do this ...
	return bits_flush(&enc->bits);
}

static int process_audio(Encoder *enc)
{
	int err;

	// Prime the analysis filters with lead-in samples before emitting output.
	enc->enable_output = 0;
	for (int i = 0; i < enc->priming_len; i++) {
		err = encode_sample(enc);
		if (err)
			return err;
	}
	enc->enable_output = 1;

	// Process samples
	while (!enc->reader_eof) {
		err = encode_sample(enc);
		if (err)
			return err;
	}

	// Flush the filters with the matching lead-out samples.
	for (int i = 0; i < enc->priming_len; i++) {
		err = encode_sample(enc);
		if (err)
			return err;
	}
	return encode_flush(enc);
}

static void reader_init(Encoder *enc, ReadSampleFunction *read, void *arg)
{
	enc->reader_func = read;
	enc->reader_arg = arg;
}

static int setup_encoder(Encoder *enc, int levels, int n_rows, const Filter *filter)
{
	enc->filter = filter;
	enc->n_levels = levels;
	enc->n_columns = 1 << levels;
	enc->n_rows = n_rows;
	enc->block_len = n_rows * enc->n_columns;

	int half_filter = (filter->filter_len + 1) / 2;
	enc->priming_len = half_filter * (enc->n_columns - 1);

	enc->sample_count = 0;
	enc->enable_output = 0;
	enc->reader_eof = 0;

	// goal: (input_pos + priming_len) % block_len == 0
	enc->input_pos = ((enc->block_len * 100) - enc->priming_len) % enc->block_len;

	enc->column_format = calloc(enc->n_columns, sizeof(*enc->column_format));
	if (enc->column_format == NULL)
		return ACM_ERR_OTHER;

	for (int i = 0; i <= levels; i++) {
		int overlap = 0;
		if (i != levels) {
			overlap = (filter->filter_len - 1) << i;
		}
		float *buf = calloc(enc->block_len + overlap, sizeof(float));
		if (buf == NULL)
			return ACM_ERR_OTHER;
		enc->level_slots[i] = buf + overlap;
	}

	return 0;
}

static void free_encoder(Encoder *enc)
{
	for (int i = 0; i <= enc->n_levels; i++) {
		if (enc->level_slots[i] != NULL) {
			int overlap = 0;
			if (enc->n_levels != i) {
				overlap = (enc->filter->filter_len - 1) << i;
			}
			free(enc->level_slots[i] - overlap);
		}
	}
	if (enc->column_format)
		free(enc->column_format);
	free(enc);
}

static int write_header(Encoder *enc, uint16_t channels, uint32_t sample_rate)
{
	FILE *out = enc->bits.out;

	if (enc->wavc) {
		enc->wavc_start_ofs = ftell(out);
		if (enc->wavc_start_ofs == -1)
			return ACM_ERR_NOT_SEEKABLE;
		OUTPUT_BITS(enc, fourcc('W', 'A', 'V', 'C'), 32);
		OUTPUT_BITS(enc, fourcc('V', '1', '.', '0'), 32);
		OUTPUT_BITS(enc, 0, 32);     // uncompr
		OUTPUT_BITS(enc, 0, 32);     // compr
		OUTPUT_BITS(enc, 7 * 4, 32); // hdrlen
		OUTPUT_BITS(enc, channels, 16);
		OUTPUT_BITS(enc, SAMPLE_WIDTH, 16);
		OUTPUT_BITS(enc, sample_rate, 32);
	}

	enc->acm_start_ofs = ftell(out);
	if (enc->acm_start_ofs == -1)
		return ACM_ERR_NOT_SEEKABLE;
	OUTPUT_BITS(enc, fourcc(0x97, 0x28, 0x03, 1), 32); // Signature + version
	OUTPUT_BITS(enc, 0, 32);			   // sample count
	OUTPUT_BITS(enc, channels, 16);
	OUTPUT_BITS(enc, sample_rate, 16);
	OUTPUT_BITS(enc, enc->n_levels, 4);
	OUTPUT_BITS(enc, enc->n_rows, 12);

	return 0;
}

static int fix_header(Encoder *enc)
{
	int err;
	FILE *out = enc->bits.out;

	long end_ofs = ftell(out);

	if (enc->wavc) {
		err = fseek(out, enc->wavc_start_ofs + 8, SEEK_SET);
		if (err)
			return ACM_ERR_OTHER;
		OUTPUT_BITS(enc, enc->sample_count * SAMPLE_WIDTH / 8, 32);
		OUTPUT_BITS(enc, end_ofs - enc->acm_start_ofs, 32);
	}

	err = fseek(out, enc->acm_start_ofs + 4, SEEK_SET);
	if (err)
		return ACM_ERR_OTHER;
	OUTPUT_BITS(enc, enc->sample_count, 32);

	err = fseek(out, end_ofs, SEEK_SET);
	if (err)
		return ACM_ERR_OTHER;

	return 0;
}

int32_t acm_encode(ReadSampleFunction *read, void *data, FILE *out, uint16_t channels,
		   uint32_t sample_rate, float volume, int levels, int samples_per_subband,
		   float comp_ratio, int wavc)
{
	if (!VALID(levels, samples_per_subband))
		return ACM_ERR_BADFMT;

	Encoder *enc = calloc(1, sizeof(Encoder));
	if (enc == NULL)
		return ACM_ERR_OTHER;

	reader_init(enc, read, data);
	bits_init(&enc->bits, out);

	int err = setup_encoder(enc, levels, samples_per_subband, &std_filter);
	if (err)
		goto error;

	enc->volume = volume;
	enc->bit_budget = (int32_t)(SAMPLE_WIDTH * enc->block_len * comp_ratio);
	enc->wavc = wavc;

	err = write_header(enc, channels, sample_rate);
	if (err)
		goto error;

	err = process_audio(enc);
	if (err)
		goto error;

	err = fix_header(enc);

error:
	free_encoder(enc);
	return err;
}
