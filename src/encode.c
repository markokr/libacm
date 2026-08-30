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
 *   1. Analysis filter bank  -- analyze() / analyze_subband().
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

#include "encode.h"

typedef struct {
	FILE *out;
	uint32_t buf;
	uint32_t count;
} BitsEncoder;

static void bits_write(BitsEncoder *bits, int32_t val, uint32_t n_bits)
{
	assert((n_bits + bits->count) <= 32);
	bits->buf |= (uint32_t)(val << bits->count);
	bits->count += n_bits;

	while (bits->count >= 8) {
		uint8_t v = bits->buf & 0xFF;
		fputc(v, bits->out);

		bits->buf >>= 8;
		bits->count -= 8;
	}
}

static void bits_flush(BitsEncoder *bits)
{
	while (bits->count >= 8) {
		uint8_t v = bits->buf & 0xFF;
		fputc(v, bits->out);

		bits->buf >>= 8;
		bits->count -= 8;
	}

	if (bits->count > 0) {
		uint8_t v = bits->buf & 0xFF;
		fputc(v, bits->out);
		bits->count = 0;
		bits->buf = 0;
	}
}

static void bits_init(BitsEncoder *bits, FILE *out)
{
	bits->out = out;
	bits->buf = 0;
	bits->count = 0;
}

typedef struct {
	float step;
	float halfStep;
	int32_t minIdx;
	int32_t maxIdx;
} Quantizer;

static void quant_init(Quantizer *q, int32_t step)
{
	q->step = (float)step;
	q->halfStep = step * 0.5f;
	q->minIdx = (int32_t)ceilf(-32767.0f / step);
	q->maxIdx = (int32_t)floorf(32767.0f / step);
}

static int32_t quant_value(Quantizer *q, float value)
{
	int32_t w = (int32_t)floorf((value + q->halfStep) / q->step);
	if (w < q->minIdx) {
		w = q->minIdx;
	} else if (w > q->maxIdx) {
		w = q->maxIdx;
	}
	return w;
}

#define FILTER_LEN 15

typedef struct {
	ReadSampleFunction *reader_func;
	void *reader_arg;
	int32_t reader_eof;

	BitsEncoder bits;

	uint32_t sample_count;
	float volume;

	int8_t n_levels;     /* decomposition depth (decoder: acm_level) */
	int32_t n_columns;   /* subband count = 1 << levels (decoder: acm_cols) */
	int32_t n_rows;	     /* samples per subband (decoder: acm_rows) */
	int32_t block_len;   /* numColumns * samples_per_subband (decoder: block_len) */
	int32_t priming_len; /* filter warm-up samples fed before/after the signal */
	float **level_slots; /* per-level coefficient buffers of the analysis tree */

	float *m_pCurrBlockData;	 /* write cursor for incoming samples (level-0 buffer) */
	int32_t m_blockSamplesRemaining; /* samples still needed to fill the current block */

	int32_t m_bandWriteEnabled;	/* 0 while priming, 1 once real output should be emitted */
	int32_t m_filterLen;		/* analysis filter length (15: symmetric, 8 unique taps) */
	uint32_t *m_pFormatIdPerColumn; /* chosen packer id per subband */
	int32_t quant_power;		/* log2 of the dequant table size (decoder: pwr) */
	int32_t quant_step;		/* uniform quantizer step size (decoder: val) */
	int32_t bit_budget;		/* target encoded size per block, in bits */
	Quantizer quantizer;
} Encoder;

static const float std_lo_filter[] = {
	-0.0012475221f, -0.0024950907f, 0.0087309526f, 0.019957958f,
	-0.050528999f,	-0.12055097f,	0.29304558f,   0.70617616f,
};

static const float std_hi_filter[] = {
	0.0012475221f, -0.0024950907f, -0.0087309526f, 0.019957958f,
	0.050528999f,  -0.12055097f,   -0.29304558f,   0.70617616f,
};

static inline int32_t codeword(Encoder *enc, int32_t row, int32_t col)
{
	float *coeffs = enc->level_slots[enc->n_levels];
	float value = coeffs[(row * enc->n_columns) + col];
	return quant_value(&enc->quantizer, value);
}

static inline int last_row(Encoder *enc, int32_t row)
{
	return row == enc->n_rows - 1;
}

typedef void (*PackFunc)(Encoder *enc, int32_t col, uint32_t formatId);

static void pack_zero(Encoder *enc, int32_t col, uint32_t formatId)
{
}

static void pack_linear(Encoder *enc, int32_t col, uint32_t formatId)
{
	int32_t mid = (1 << (formatId - 1));
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		bits_write(&enc->bits, w + mid, formatId);
	}
}

/* Words {-1..1}, assume zero pair */
static void pack_peak1zz(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (!last_row(enc, row) && codeword(enc, row + 1, col) == 0) {
				/* 0 */
				bits_write(&enc->bits, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				bits_write(&enc->bits, 1, 2);
			}
		} else {
			/* 1, 1, ? */
			bits_write(&enc->bits, 3, 2);
			bits_write(&enc->bits, (w == 1) ? 1 : 0, 1);
		}
	}
}

/* Words {-1..1}, assume zero */
static void pack_peak1z(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			/* 0 */
			bits_write(&enc->bits, 0, 1);
		} else {
			/* 1, ? */
			bits_write(&enc->bits, 1, 1);
			bits_write(&enc->bits, (w == 1) ? 1 : 0, 1);
		}
	}
}

/* Base-3 packing: 3 indices in {-1,0,1} per 5-bit word (decoder: f_t15). */
static void pack_base3(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		int32_t packed = w + 1;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 1) * 3;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 1) * 9;

		bits_write(&enc->bits, packed, 5);
	}
}

/* Words {-2..2}, assume zero pair */
static void pack_peak2zz(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (!last_row(enc, row) && codeword(enc, row + 1, col) == 0) {
				/* 0 */
				bits_write(&enc->bits, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				bits_write(&enc->bits, 1, 2);
			}
		} else {
			/* 1, 1, ?, ? */
			bits_write(&enc->bits, 3, 2);
			if (w < 0) {
				w += 2;
			} else {
				w += 1;
			}
			bits_write(&enc->bits, w, 2);
		}
	}
}

/* Words {-2..2}, assume zero */
static void pack_peak2(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);

		if (w == 0) {
			/* 0 */
			bits_write(&enc->bits, 0, 1);
		} else {
			/* 1, ?, ? */
			bits_write(&enc->bits, 1, 1);
			if (w < 0) {
				w += 2;
			} else {
				w += 1;
			}
			bits_write(&enc->bits, w, 2);
		}
	}
}

/* Base-5 packing: 3 words in {-2..2} per 7-bits */
static void pack_base5(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		int32_t packed = w + 2;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 2) * 5;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 2) * 25;

		bits_write(&enc->bits, packed, 7);
	}
}

/* Words {-3..3}, assume zero pair */
static void pack_peak3zz(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (!last_row(enc, row) && codeword(enc, row + 1, col) == 0) {
				/* 0 */
				bits_write(&enc->bits, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				bits_write(&enc->bits, 1, 2);
			}
		} else {
			/* 1, 1 */
			bits_write(&enc->bits, 3, 2);
			if (w != -1 && w != 1) {
				/* 1, ?, ? */
				bits_write(&enc->bits, 1, 1);
				if (w < 0) {
					w += 3;
				}
				bits_write(&enc->bits, w, 2);
			} else {
				/* 0, ? */
				bits_write(&enc->bits, 0, 1);
				bits_write(&enc->bits, (w == 1) ? 1 : 0, 1);
			}
		}
	}
}

/* Words in {-3..3}, assume zero */
static void pack_peak3z(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			/* 0 */
			bits_write(&enc->bits, 0, 1);
		} else {
			bits_write(&enc->bits, 1, 1);
			if (w != -1 && w != 1) {
				/* 1, 1, ?, ? */
				bits_write(&enc->bits, 1, 1);
				if (w < 0) {
					w += 3;
				}
				bits_write(&enc->bits, w, 2);
			} else {
				/* 1, 0, ?, ? */
				bits_write(&enc->bits, 0, 1);
				bits_write(&enc->bits, (w == 1) ? 1 : 0, 1);
			}
		}
	}
}

/* Words in {-4..4}, assume zero pair */
static void pack_peak4zz(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		if (w == 0) {
			if (!last_row(enc, row) && codeword(enc, row + 1, col) == 0) {
				/* 0 */
				bits_write(&enc->bits, 0, 1);
				row++;
			} else {
				/* 1, 0 */
				bits_write(&enc->bits, 1, 2);
			}
		} else {
			bits_write(&enc->bits, 3, 2);
			if (w >= 0) {
				w += 3;
			} else {
				w += 4;
			}
			bits_write(&enc->bits, w, 3);
		}
	}
}

/* Words {-4..4}, assume zero */
static void pack_peak4z(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);

		if (w == 0) {
			bits_write(&enc->bits, 0, 1);
		} else {
			bits_write(&enc->bits, 1, 1);
			if (w < 0) {
				w += 4;
			} else {
				w += 3;
			}
			bits_write(&enc->bits, w, 3);
		}
	}
}

/* Base-11 packing: 2 words in {-5..5} per 7-bits */
static void pack_base11(Encoder *enc, int32_t col, uint32_t formatId)
{
	for (int32_t row = 0; row < enc->n_rows; row++) {
		int32_t w = codeword(enc, row, col);
		int32_t packed = w + 5;

		w = last_row(enc, row) ? 0 : codeword(enc, ++row, col);
		packed += (w + 5) * 11;

		bits_write(&enc->bits, packed, 7);
	}
}

enum PackerId {
	Zero,
	Linear3 = 3,
	Linear16 = 16,

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
};

/*
 * Packer dispatch table, indexed by the 5-bit format id.  Index-aligned with
 * the decoder's filler_list[] so id N encodes exactly what f_*(N) decodes.
 */
static const PackFunc packer_list[] = {
	pack_zero,    NULL,	    NULL,	  pack_linear,	/* 0 .. 3 */
	pack_linear,  pack_linear,  pack_linear,  pack_linear,	/* 4 .. 7 */
	pack_linear,  pack_linear,  pack_linear,  pack_linear,	/* 8 .. 11 */
	pack_linear,  pack_linear,  pack_linear,  pack_linear,	/* 12 .. 15 */
	pack_linear,  pack_peak1zz, pack_peak1z,  pack_base3,	/* 16 .. 19 */
	pack_peak2zz, pack_peak2,   pack_base5,	  pack_peak3zz, /* 20 .. 23 */
	pack_peak3z,  NULL,	    pack_peak4zz, pack_peak4z,	/* 24 .. 27 */
	NULL,	      pack_base11,  NULL,	  NULL		/* 28 .. 31 */
};

static void reader_init(Encoder *enc, ReadSampleFunction *read, void *data)
{
	enc->reader_func = read;
	enc->reader_arg = data;
}

static int setup_encoder(Encoder *enc, int filterLen, int8_t levels, int samples_per_subband)
{
	enc->m_filterLen = filterLen;
	enc->n_levels = levels;
	enc->n_columns = 1 << levels;
	enc->n_rows = samples_per_subband;
	enc->block_len = samples_per_subband * enc->n_columns;

	int halfFilter = (((filterLen < -1) ? (filterLen + 1) : (filterLen)) + 1) >> 1;
	enc->priming_len = halfFilter * (enc->n_columns - 1);
	enc->level_slots = (float **)malloc(sizeof(float *) * (levels + 1));
	if (enc->level_slots == NULL)
		return 0;

	if (levels >= 0) {
		for (int8_t i = 0; i <= levels; ++i) {
			int overlap = 0;
			if (i != levels) {
				overlap = (filterLen - 1) << i;
			}

			float *buf = (float *)malloc((enc->block_len + overlap) * sizeof(float));
			enc->level_slots[i] = buf;
			if (buf == NULL)
				return 0;

			memset(buf, 0, (enc->block_len + overlap) * sizeof(float));
			enc->level_slots[i] += overlap;
		}
	}

	enc->m_pFormatIdPerColumn = (uint32_t *)malloc(enc->n_columns * sizeof(uint32_t));
	if (enc->m_pFormatIdPerColumn == NULL)
		return 0;

	enc->sample_count = 0;

	int32_t startOffset =
	    ((enc->block_len * sizeof(float) * 25) - enc->priming_len) % enc->block_len;
	enc->m_pCurrBlockData = enc->level_slots[0] + startOffset;
	enc->m_blockSamplesRemaining = enc->block_len - startOffset;
	enc->m_bandWriteEnabled = 0;
	enc->reader_eof = 0;
	return 1;
}

static void destroy_encoder(Encoder *enc)
{
	if (enc->level_slots != NULL) {
		for (int i = 0; i <= enc->n_levels; ++i) {
			if (enc->level_slots[i] != 0) {
				int overlap = 0;
				if (enc->n_levels != i) {
					overlap = (enc->m_filterLen - 1) << i;
				}

				free(enc->level_slots[i] - overlap);
			}
		}

		free(enc->level_slots);
	}

	if (enc->m_pFormatIdPerColumn != NULL) {
		free(enc->m_pFormatIdPerColumn);
	}
}

/*
 * One 2-channel QMF analysis step over a single subband.  Symmetric FIR:
 * even outputs use the low-pass (scaling) filter, odd outputs the high-pass
 * (wavelet) filter, giving a critically-sampled low/high split.  Samples of
 * this subband are interleaved in the buffer with distance `stride`.
 */
static void analyze_subband(Encoder *enc, float *src, float *dst, int32_t stride, int32_t count)
{
	if (count <= 0)
		return;

	const int32_t halfTaps = (enc->m_filterLen - 1) >> 1; /* taps on each side of center */
	const int32_t reach = halfTaps * stride;	      /* offset to the symmetric neighbor */
	src -= reach;

	for (int i = 0; i < count; ++i) {
		const float *coef = (i & 1) ? std_hi_filter : std_lo_filter;
		float *pLeft = src - reach;
		float *pRight = src + reach;

		float acc = 0.0f;
		if (halfTaps > 0) {
			for (int32_t k = halfTaps; k != 0; --k) {
				acc += (*pRight + *pLeft) * *coef++;
				pLeft += stride;
				pRight -= stride;
			}
		}

		*dst = (*pLeft * *coef) + acc;
		dst += stride;
		src += stride;
	}
}

/*
 * Forward analysis filter bank: a dyadic tree of 2-channel QMF stages that
 * splits the block into n_columns == 2^levels uniform subbands.  Inverse
 * of the decoder's juggle_block().
 */
static void analyze(Encoder *enc)
{
	if (enc->n_levels <= 0)
		return;

	int32_t stride = 1;		/* subbands produced so far == interleave stride */
	int32_t count = enc->block_len; /* samples per subband at this level */
	for (int i = 0; i < enc->n_levels; ++i) {
		float *src = enc->level_slots[i];
		float *dst = enc->level_slots[i + 1];

		for (int band = 0; band < stride; ++band) {
			analyze_subband(enc, src++, dst++, stride, count);
		}

		stride += stride;
		count >>= 1;
	}
}

/*
 * Rate estimation and per-subband format selection for a candidate quant step.
 * Quantizes every coefficient with q = floor((x + step/2) / step) (clamped),
 * finds the peak index per subband, picks the cheapest packer for it, and
 * returns the total encoded size of the block in bits.  Side effects: fills
 * m_pFormatIdPerColumn[] and records quant_power / quant_step.
 */
static int32_t estimate_bits(Encoder *enc, int32_t step)
{
	/* Uniform packers keyed by peak magnitude */
	static const uint32_t flat_fmt[] = { Zero, Peak1Base3, Peak2Base5, Linear3, Peak5Base11 };

	int32_t quantPower = 3;
	int32_t bits = 4 + 16 + enc->n_columns * 5; /* header bits */

	float *coeffs = enc->level_slots[enc->n_levels];
	float *colBase = enc->level_slots[enc->n_levels];

	quant_init(&enc->quantizer, step);

	for (int32_t col = 0; col < enc->n_columns; col++) {
		int32_t minWord = 0x10000;
		int32_t maxWord = -0x10000;
		if (enc->n_rows > 0) {
			for (int row = 0; row < enc->n_rows; row++) {
				int32_t w = codeword(enc, row, col);
				if (w < minWord) {
					minWord = w;
				}
				if (w > maxWord) {
					maxWord = w;
				}
			}
		}

		int32_t absPeak = abs(minWord);
		if (absPeak < maxWord) {
			absPeak = maxWord;
		} else if (absPeak < -maxWord) {
			absPeak = -maxWord;
		}

		int32_t cost;
		if (absPeak == 0) {
			cost = 0;
			enc->m_pFormatIdPerColumn[col] = Zero;
		} else if (absPeak <= 4) {
			int32_t tailBits = 1;
			int32_t runFmt = absPeak * 3 + Peak1ZZ - 3; /* Peak1Z,2,3,4*/
			if (absPeak != 1) {
				tailBits = ((absPeak - 2) < 1) ? 2 : 3;
			}

			/* costA vs costB: the two run-length orientations of the k-code */
			int32_t costPaired = 0;
			int32_t costUnpaired = 0;
			for (int row = 0; row < enc->n_rows; row++) {
				int32_t w = codeword(enc, row, col);
				if (w != 0) {
					if (tailBits != 1) {
						if (w == -1 || w == 1) {
							costPaired += 4;
							costUnpaired += 3;
						} else {
							costPaired += tailBits + 2;
							costUnpaired += tailBits + 1;
						}
					} else {
						costPaired += 3;
						costUnpaired += 2;
					}
				} else if (last_row(enc, row)) {
					costPaired += 2;
					costUnpaired += 1;
				} else {
					int32_t next = codeword(enc, row + 1, col);
					if (next != 0) {
						costPaired += 2;
						costUnpaired += 1;
					} else {
						costPaired += 1;
						costUnpaired += 2;
						row++;
					}
				}
			}

			if (costPaired > costUnpaired) {
				cost = costUnpaired;
				runFmt += 1;
			} else {
				cost = costPaired;
			}

			int32_t flatBits;
			if (absPeak != 4) {
				/* triplets */
				flatBits = ((enc->n_rows + 2) / 3) * ((absPeak * 2) + 3);
			} else {
				/* base11 pairs */
				flatBits =
				    (((enc->n_rows < -1) ? (enc->n_rows + 2) : (enc->n_rows + 1))
				     >> 1)
				    * 7;
			}

			if (flatBits < cost) {
				cost = flatBits;
				runFmt = flat_fmt[absPeak];
			}

			enc->m_pFormatIdPerColumn[col] = runFmt;
		} else if (minWord >= -5 && maxWord <= 5) {
			if ((enc->n_rows < -1)) {
				cost = ((enc->n_rows + 2) >> 1) * 7;
			} else {
				cost = ((enc->n_rows + 1) >> 1) * 7;
			}
			enc->m_pFormatIdPerColumn[col] = Peak5Base11;
		} else {
			/* Fixed-width "linear": pick the bit width that spans the index range */
			int32_t mag = 0;
			if (minWord < 0) {
				mag = ~minWord;
			}

			if (maxWord > 0 && ((uint32_t)(mag) < (uint32_t)(maxWord))) {
				mag = maxWord;
			}

			int32_t nbits = 1;
			while (mag) {
				mag >>= 1;
				++nbits;
			}

			if (quantPower < (nbits - 1)) {
				quantPower = nbits - 1;
			}
			enc->m_pFormatIdPerColumn[col] = nbits; /* linear: format id == bit width */
			cost = nbits * enc->n_rows;
		}

		bits += cost;
		++colBase;
	}

	enc->quant_power = quantPower;
	enc->quant_step = step;
	quant_init(&enc->quantizer, step);

	return bits;
}

/*
 * Rate control: binary-search the quant step so the encoded block is the
 * largest that still fits within bit_budget (a smaller step is finer and
 * costs more bits).
 */
static void choose_quant_step(Encoder *enc)
{
	int32_t lo = 1, hi = 0x7FFF;
	do {
		int32_t mid = (lo + hi) >> 1;
		int32_t bits = estimate_bits(enc, mid);
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

static void write_bands(Encoder *enc)
{
	bits_write(&enc->bits, enc->quant_power, 4);
	bits_write(&enc->bits, enc->quant_step, 16);

	for (int i = 0; i < enc->n_columns; ++i) {
		const uint32_t formatId = enc->m_pFormatIdPerColumn[i];
		bits_write(&enc->bits, formatId, 5);
		packer_list[formatId](enc, i, formatId);
	}
}

/*
 * Carry the filter-overlap tail of each level into the next block so the
 * analysis filters stay continuous across block boundaries.
 */
static void shift_overlap(Encoder *enc)
{
	int32_t overlap = enc->m_filterLen - 1;
	for (int i = 0; i < enc->n_levels; ++i, overlap += overlap) {
		float *pDst = enc->level_slots[i] - overlap;
		float *pSrc = pDst + enc->block_len;
		memcpy(pDst, pSrc, overlap * sizeof(float));
	}
}

static void process_block(Encoder *enc)
{
	analyze(enc);

	if (enc->m_bandWriteEnabled) {
		choose_quant_step(enc);
		write_bands(enc);
	}

	shift_overlap(enc);

	enc->m_blockSamplesRemaining += enc->block_len;
	enc->m_pCurrBlockData -= enc->block_len;
}

static void encode_sample(Encoder *enc)
{
	int32_t sample = 0;
	if (enc->reader_eof == 0) {
		sample = (*enc->reader_func)(enc->reader_arg);
		if (sample == (int32_t)ReadSampleEof) {
			enc->reader_eof = 1;
			return;
		}

		++enc->sample_count;
	}

	*enc->m_pCurrBlockData++ = enc->volume * (float)sample;

	if (--enc->m_blockSamplesRemaining == 0) {
		process_block(enc);
	}
}

static void encode_flush(Encoder *enc)
{
	if (enc->block_len == enc->m_blockSamplesRemaining) {
		// no data in the block
		return;
	}

	// Zero out the remaining data in the block
	while (enc->m_blockSamplesRemaining != 0) {
		*enc->m_pCurrBlockData++ = 0.0f;
		--enc->m_blockSamplesRemaining;
	}

	// Send it off for processing
	process_block(enc);

	/////////////
	// NOTE: The Interplay one doesn't do this ... but it should as there
	// may be bits left in the bit processor that should go out
	bits_flush(&enc->bits);
	/////////////
}

int32_t acm_encode(ReadSampleFunction *read, void *data, FILE *out, unsigned channels,
		   unsigned sample_rate, float volume, int levels, int samples_per_subband,
		   float comp_ratio)
{
	Encoder enc;
	memset(&enc, 0, sizeof(enc));

	int32_t startPos = ftell(out);
	reader_init(&enc, read, data);
	bits_init(&enc.bits, out);
	enc.volume = volume;

	if (!setup_encoder(&enc, FILTER_LEN, levels, samples_per_subband)) {
		destroy_encoder(&enc);
		return 0;
	}

	enc.bit_budget = (int32_t)(16.0 * enc.block_len * comp_ratio);

	bits_write(&enc.bits, 0x032897, 24); // Signature
	bits_write(&enc.bits, 1, 8);	     // Version

	bits_write(&enc.bits, enc.sample_count, 32); // Placeholder
	bits_write(&enc.bits, channels, 16);
	bits_write(&enc.bits, sample_rate, 16);
	bits_write(&enc.bits, levels, 4);
	bits_write(&enc.bits, samples_per_subband, 12);

	// Prime the analysis filters with lead-in samples before emitting output.
	int32_t primeCount = enc.priming_len;
	enc.m_bandWriteEnabled = 0;
	while (primeCount) {
		encode_sample(&enc);
		--primeCount;
	}
	enc.m_bandWriteEnabled = 1;

	// Process samples
	while (!enc.reader_eof) {
		encode_sample(&enc);
	}

	// Flush the filters with the matching lead-out samples.
	primeCount = enc.priming_len;
	while (primeCount) {
		encode_sample(&enc);
		--primeCount;
	}

	encode_flush(&enc);

	// Go back and write the Sample Count out proper
	fseek(out, startPos + 4, SEEK_SET);
	bits_write(&enc.bits, enc.sample_count, 32);
	fseek(out, 0, SEEK_END);

	destroy_encoder(&enc);
	return 1;
}
