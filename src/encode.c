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
 *      into m_numColumns == 2^levels uniform subbands.  std_lo_filter is the
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
	FILE *m_outFile;
	uint32_t m_bitData;
	uint32_t m_bitCount;
} BitsEncoder;

static void bits_write(BitsEncoder *bits, int32_t val, uint32_t numBits)
{
	assert((numBits + bits->m_bitCount) <= 32);
	bits->m_bitData |= (uint32_t)(val << bits->m_bitCount);
	bits->m_bitCount += numBits;

	while (bits->m_bitCount >= 8) {
		uint8_t v = bits->m_bitData & 0xFF;
		putc(v, bits->m_outFile);

		bits->m_bitData >>= 8;
		bits->m_bitCount -= 8;
	}
}

static void bits_flush(BitsEncoder *bits)
{
	while (bits->m_bitCount >= 8) {
		uint8_t v = bits->m_bitData & 0xFF;
		putc(v, bits->m_outFile);

		bits->m_bitData >>= 8;
		bits->m_bitCount -= 8;
	}

	if (bits->m_bitCount > 0) {
		uint8_t v = bits->m_bitData & 0xFF;
		putc(v, bits->m_outFile);
		bits->m_bitCount = 0;
		bits->m_bitData = 0;
	}
}

static void bits_init(BitsEncoder *bits, FILE *out)
{
	bits->m_outFile = out;
	bits->m_bitData = 0;
	bits->m_bitCount = 0;
}

typedef struct {
	ReadSampleFunction *m_reader;
	void *m_pReaderData;
	uint32_t m_sampleCount;
	float m_volume;
	BitsEncoder m_bits;
	int8_t m_levels;		 /* decomposition depth (decoder: acm_level) */
	int32_t m_numColumns;		 /* subband count = 1 << levels (decoder: acm_cols) */
	int32_t m_samples_per_subband;	 /* samples per subband (decoder: acm_rows) */
	int32_t m_samplesPerBlock;	 /* numColumns * samples_per_subband (decoder: block_len) */
	int32_t m_primingLen;		 /* filter warm-up samples fed before/after the signal */
	float **m_levelSlots;		 /* per-level coefficient buffers of the analysis tree */
	float *m_pCurrBlockData;	 /* write cursor for incoming samples (level-0 buffer) */
	int32_t m_blockSamplesRemaining; /* samples still needed to fill the current block */
	int32_t m_bandWriteEnabled;	 /* 0 while priming, 1 once real output should be emitted */
	int32_t m_finishedReading;	 /* set once the input reader hits EOF */
	int32_t m_filterLen;		 /* analysis filter length (15: symmetric, 8 unique taps) */
	const float *m_lo_filter;	 /* analysis low-pass (scaling) filter */
	const float *m_hi_filter;	 /* analysis high-pass (wavelet) filter */
	uint32_t *m_pFormatIdPerColumn;	 /* chosen packer id per subband */
	int32_t m_quantPower;		 /* log2 of the dequant table size (decoder: pwr) */
	int32_t m_quantStep;		 /* uniform quantizer step size (decoder: val) */
	int32_t m_bitBudget;		 /* target encoded size per block, in bits */
} Encoder;

static const float std_lo_filter[] = {
	-0.0012475221f, -0.0024950907f, 0.0087309526f, 0.019957958f,
	-0.050528999f,	-0.12055097f,	0.29304558f,   0.70617616f,
};

static const float std_hi_filter[] = {
	0.0012475221f, -0.0024950907f, -0.0087309526f, 0.019957958f,
	0.050528999f,  -0.12055097f,   -0.29304558f,   0.70617616f,
};

typedef void (*PackFunc)(Encoder *enc, int32_t col, uint32_t formatId);

/* All-zero subband: nothing to emit beyond the format id (decoder: f_zero). */
static void pack_zero(Encoder *enc, int32_t col, uint32_t formatId)
{
}

/* Fixed-width PCM index, formatId bits each, biased by half its range (decoder: f_linear). */
static void pack_linear(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	float *p = &enc->m_levelSlots[enc->m_levels][col];
	for (int32_t i = 0; i < enc->m_samples_per_subband; ++i) {
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (idx > maxIdx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		bits_write(&enc->m_bits, idx + (1 << (formatId - 1)), formatId);
	}
}

/* Run-length code, peak 1, zero-run pairing (decoder: f_k13). */
static void pack_k13(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		if (idx == 0) {
			if (n != 0 && !(int)floorf((*p + halfStep) / step)) {
				bits_write(&enc->m_bits, 0, 1);

				if (n == 0)
					return;
				--n;
				p += enc->m_numColumns;
				continue;
			}

			bits_write(&enc->m_bits, 1, 2);
			continue;
		}

		bits_write(&enc->m_bits, 3, 2);
		bits_write(&enc->m_bits, (idx == 1) ? 1 : 0, 1);
	}
}

/* Run-length code, peak 1, no zero-run pairing (decoder: f_k12). */
static void pack_k12(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (idx > maxIdx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;

		if (!idx) {
			bits_write(&enc->m_bits, 0, 1);
			continue;
		}

		bits_write(&enc->m_bits, 1, 1);
		bits_write(&enc->m_bits, (idx == 1) ? 1 : 0, 1);
	}
}

/* Base-3 packing: 3 indices in {-1,0,1} per 5-bit word (decoder: f_t15). */
static void pack_t15(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}
		p += enc->m_numColumns;
		int32_t packed = idx + 1;
		if (n) {
			--n;
			idx = (int32_t)floorf((*p + halfStep) / step);
			if (minIdx > idx) {
				idx = minIdx;
			} else if (maxIdx < idx) {
				idx = maxIdx;
			}
			p += enc->m_numColumns;
		} else {
			idx = 0;
		}

		packed += idx * 3 + 3;
		if (n) {
			--n;
			idx = (int32_t)floorf((*p + halfStep) / step);
			if (minIdx > idx) {
				idx = minIdx;
			} else if (maxIdx < idx) {
				idx = maxIdx;
			}
			p += enc->m_numColumns;
		} else {
			idx = 0;
		}

		bits_write(&enc->m_bits, idx * 9 + 9 + packed, 5);
	}
}

/* Run-length code, peak 2, zero-run pairing (decoder: f_k24). */
static void pack_k24(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		if (idx == 0) {
			if (n != 0 && !(int32_t)floorf((*p + halfStep) / step)) {
				bits_write(&enc->m_bits, 0, 1);

				if (n == 0)
					return;
				--n;
				p += enc->m_numColumns;
				continue;
			}
			bits_write(&enc->m_bits, 1, 2);
			continue;
		}
		bits_write(&enc->m_bits, 3, 2);
		if (idx < 0) {
			idx += 2;
		} else {
			++idx;
		}

		bits_write(&enc->m_bits, idx, 2);
	}
}

/* Run-length code, peak 2, no zero-run pairing (decoder: f_k23). */
static void pack_k23(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		if (!idx) {
			bits_write(&enc->m_bits, 0, 1);
			continue;
		}
		bits_write(&enc->m_bits, 1, 1);

		if (idx < 0) {
			idx += 2;
		} else {
			++idx;
		}

		bits_write(&enc->m_bits, idx, 2);
	}
}

/* Base-5 packing: 3 indices in {-2..2} per 7-bit word (decoder: f_t27). */
static void pack_t27(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];

	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		int32_t packed = idx + 2;
		p += enc->m_numColumns;
		if (n) {
			--n;
			idx = (int32_t)floorf((*p + halfStep) / step);
			if (minIdx > idx) {
				idx = minIdx;
			} else if (maxIdx < idx) {
				idx = maxIdx;
			}
			p += enc->m_numColumns;
		} else {
			idx = 0;
		}

		packed += idx * 5 + 10;
		if (n) {
			--n;
			idx = (int32_t)floorf((*p + halfStep) / step);
			if (minIdx > idx) {
				idx = minIdx;
			} else if (maxIdx < idx) {
				idx = maxIdx;
			}
			p += enc->m_numColumns;
		} else {
			idx = 0;
		}

		bits_write(&enc->m_bits, idx * 25 + 50 + packed, 7);
	}
}

/* Run-length code, peak 3, zero-run pairing (decoder: f_k35). */
static void pack_k35(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;

		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		if (!idx) {
			if (n != 0) {
				if (!(int32_t)floorf((*p + halfStep) / step)) {
					bits_write(&enc->m_bits, 0, 1);

					if (n == 0)
						return;
					--n;

					p += enc->m_numColumns;
					continue;
				}
			}

			bits_write(&enc->m_bits, 1, 2);
			continue;
		}

		bits_write(&enc->m_bits, 3, 2);

		if (idx != -1 && idx != 1) {
			bits_write(&enc->m_bits, 1, 1);

			if (idx < 0) {
				idx += 3;
			}

			bits_write(&enc->m_bits, idx, 2);
			continue;
		}

		bits_write(&enc->m_bits, 0, 1);
		bits_write(&enc->m_bits, (idx == 1) ? 1 : 0, 1);
	}
}

/* Run-length code, peak 3, no zero-run pairing (decoder: f_k34). */
static void pack_k34(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		if (!idx) {
			bits_write(&enc->m_bits, 0, 1);
			continue;
		}

		bits_write(&enc->m_bits, 1, 1);

		if (idx != -1 && idx != 1) {
			bits_write(&enc->m_bits, 1, 1);

			if (idx < 0) {
				idx += 3;
			}
			bits_write(&enc->m_bits, idx, 2);
			continue;
		}

		bits_write(&enc->m_bits, 0, 1);
		bits_write(&enc->m_bits, (idx == 1) ? 1 : 0, 1);
	}
}

/* Run-length code, peak 4, zero-run pairing (decoder: f_k45). */
static void pack_k45(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)(ceilf(-32767.0f / step));
	const int32_t maxIdx = (int32_t)(floorf(32767.0f / step));
	const float *p = &enc->m_levelSlots[enc->m_levels][col];

	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;

		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		if (!idx) {
			if (n) {
				int32_t next = (int32_t)floorf((*p + halfStep) / step);
				if (!next) {
					bits_write(&enc->m_bits, 0, 1);

					if (n == 0)
						return;

					--n;
					p += enc->m_numColumns;
					continue;
				}
			}

			bits_write(&enc->m_bits, 1, 2);
			continue;
		}

		bits_write(&enc->m_bits, 3, 2);

		if (idx >= 0) {
			idx += 3;
		} else {
			idx += 4;
		}

		bits_write(&enc->m_bits, idx, 3);
	}
}

/* Run-length code, peak 4, no zero-run pairing (decoder: f_k44). */
static void pack_k44(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)(enc->m_quantStep);
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;
		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (idx < minIdx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		p += enc->m_numColumns;
		if (!idx) {
			bits_write(&enc->m_bits, 0, 1);
			continue;
		}

		bits_write(&enc->m_bits, 1, 1);

		if (idx < 0) {
			idx += 4;
		} else {
			idx += 3;
		}

		bits_write(&enc->m_bits, idx, 3);
	}
}

/* Base-11 packing: 2 indices in {-5..5} per 7-bit word (decoder: f_t37). */
static void pack_t37(Encoder *enc, int32_t col, uint32_t formatId)
{
	const float step = (float)enc->m_quantStep;
	const float halfStep = step * 0.5f;
	const int32_t minIdx = (int32_t)ceilf(-32767.0f / step);
	const int32_t maxIdx = (int32_t)floorf(32767.0f / step);

	const float *p = &enc->m_levelSlots[enc->m_levels][col];
	int32_t n = enc->m_samples_per_subband;
	while (n) {
		--n;

		int32_t idx = (int32_t)floorf((*p + halfStep) / step);
		if (minIdx > idx) {
			idx = minIdx;
		} else if (maxIdx < idx) {
			idx = maxIdx;
		}

		int32_t packed = idx + 5;
		p += enc->m_numColumns;
		if (n != 0) {
			--n;

			idx = (int32_t)floorf((*p + halfStep) / step);
			if (minIdx > idx) {
				idx = minIdx;
			} else if (maxIdx < idx) {
				idx = maxIdx;
			}

			p += enc->m_numColumns;
		} else {
			idx = 0;
		}

		bits_write(&enc->m_bits, 11 * idx + 55 + packed, 7);
	}
}

/*
 * Packer dispatch table, indexed by the 5-bit format id.  Index-aligned with
 * the decoder's filler_list[] so id N encodes exactly what f_*(N) decodes.
 */
static const PackFunc packer_list[] = { pack_zero,
					NULL,
					NULL,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_linear,
					pack_k13,
					pack_k12,
					pack_t15,
					pack_k24,
					pack_k23,
					pack_t27,
					pack_k35,
					pack_k34,
					NULL,
					pack_k45,
					pack_k44,
					NULL,
					pack_t37,
					NULL,
					NULL };

static void ReadSample_init(Encoder *enc, ReadSampleFunction *read, void *data)
{
	enc->m_reader = read;
	enc->m_pReaderData = data;
}

static int SetupEncoder(Encoder *enc, int filterLen, const float lo_filter[],
			const float hi_filter[], int8_t levels, int samples_per_subband)
{
	enc->m_filterLen = filterLen;
	enc->m_lo_filter = lo_filter;
	enc->m_hi_filter = hi_filter;
	enc->m_levels = levels;
	enc->m_numColumns = 1 << levels;
	enc->m_samples_per_subband = samples_per_subband;
	enc->m_samplesPerBlock = samples_per_subband * enc->m_numColumns;

	int halfFilter = (((filterLen < -1) ? (filterLen + 1) : (filterLen)) + 1) >> 1;
	enc->m_primingLen = halfFilter * (enc->m_numColumns - 1);
	enc->m_levelSlots = (float **)malloc(sizeof(float *) * (levels + 1));
	if (enc->m_levelSlots == NULL)
		return 0;

	if (levels >= 0) {
		for (int8_t i = 0; i <= levels; ++i) {
			int overlap = 0;
			if (i != levels) {
				overlap = (filterLen - 1) << i;
			}

			float *buf = (float *)malloc((enc->m_samplesPerBlock + overlap) * sizeof(float));
			enc->m_levelSlots[i] = buf;
			if (buf == NULL)
				return 0;

			memset(buf, 0, (enc->m_samplesPerBlock + overlap) * sizeof(float));
			enc->m_levelSlots[i] += overlap;
		}
	}

	enc->m_pFormatIdPerColumn = (uint32_t *)malloc(enc->m_numColumns * sizeof(uint32_t));
	if (enc->m_pFormatIdPerColumn == NULL)
		return 0;

	enc->m_sampleCount = 0;

	int32_t startOffset =
	    ((enc->m_samplesPerBlock * sizeof(float) * 25) - enc->m_primingLen)
	    % enc->m_samplesPerBlock;
	enc->m_pCurrBlockData = enc->m_levelSlots[0] + startOffset;
	enc->m_blockSamplesRemaining = enc->m_samplesPerBlock - startOffset;
	enc->m_bandWriteEnabled = 0;
	enc->m_finishedReading = 0;
	return 1;
}

static void DestroyEncoder(Encoder *enc)
{
	if (enc->m_levelSlots != NULL) {
		for (int i = 0; i <= enc->m_levels; ++i) {
			if (enc->m_levelSlots[i] != 0) {
				int overlap = 0;
				if (enc->m_levels != i) {
					overlap = (enc->m_filterLen - 1) << i;
				}

				free(enc->m_levelSlots[i] - overlap);
			}
		}

		free(enc->m_levelSlots);
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
		const float *coef = (i & 1) ? enc->m_hi_filter : enc->m_lo_filter;
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
 * splits the block into m_numColumns == 2^levels uniform subbands.  Inverse
 * of the decoder's juggle_block().
 */
static void analyze(Encoder *enc)
{
	if (enc->m_levels <= 0)
		return;

	int32_t stride = 1;			 /* subbands produced so far == interleave stride */
	int32_t count = enc->m_samplesPerBlock;	 /* samples per subband at this level */
	for (int i = 0; i < enc->m_levels; ++i) {
		float *src = enc->m_levelSlots[i];
		float *dst = enc->m_levelSlots[i + 1];

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
 * m_pFormatIdPerColumn[] and records m_quantPower / m_quantStep.
 */
static int32_t estimate_bits(Encoder *enc, int32_t step)
{
	/* Ternary packer id keyed by peak magnitude: t15, t27, (unused), t37. */
	static const uint32_t ternary_fmt[] = { 0x00, 0x13, 0x16, 0x03, 0x1D, 0x00 };

	int32_t quantPower = 3;
	int32_t bits = enc->m_numColumns * 5 + 20; /* 5-bit id per subband + 4+16 block header */

	float halfStep = (float)step * 0.5f;
	// Clamp indices to the range pack_linear actually emits.  Without this,
	// a coefficient whose index exceeds +-32767 makes estimate_bits pick a
	// formatId >= 17 (which collides with the packed-format ids: 17 == k13)
	// and drives quantPower to 16, overflowing the 4-bit power field in the
	// block header.
	const int32_t minClamp = (int32_t)ceilf(-32767.0f / (float)step);
	const int32_t maxClamp = (int32_t)floorf(32767.0f / (float)step);
	float *coeffs = enc->m_levelSlots[enc->m_levels];
	float *colBase = enc->m_levelSlots[enc->m_levels];

	for (int32_t col = 0; col < enc->m_numColumns; ++col) {
		int32_t minIdx = 0x10000;
		int32_t maxIdx = -0x10000;
		if (enc->m_samples_per_subband > 0) {
			float *p = colBase;
			for (int row = enc->m_samples_per_subband; row != 0; --row) {
				int32_t idx = (int32_t)floor((*p + halfStep) / (float)step);
				if (idx < minClamp) {
					idx = minClamp;
				} else if (idx > maxClamp) {
					idx = maxClamp;
				}
				if (minIdx > idx) {
					minIdx = idx;
				}

				if (maxIdx < idx) {
					maxIdx = idx;
				}

				p += enc->m_numColumns;
			}
		}

		int32_t absPeak = abs(minIdx);
		if (absPeak < maxIdx) {
			absPeak = maxIdx;
		} else if (absPeak < -maxIdx) {
			absPeak = -maxIdx;
		}

		int32_t cost;
		if (absPeak == 0) {
			cost = 0;
			enc->m_pFormatIdPerColumn[col] = 0;
		} else if (absPeak <= 4) {
			int32_t tailBits = 1;
			int32_t runFmt = absPeak * 3 + 14; /* k13 / k24 / k35 / k45 */
			if (absPeak != 1) {
				tailBits = ((absPeak - 2) < 1) ? 2 : 3;
			}

			/* costA vs costB: the two run-length orientations of the k-code */
			int32_t costA = 0;
			int32_t costB = 0;
			for (int row = 0; row < enc->m_samples_per_subband; ++row) {
				int32_t idx =
				    (int)floor((coeffs[(row * enc->m_numColumns) + col] + halfStep)
					       / (float)step);
				if (idx) {
					if (tailBits != 1) {
						if (idx == -1 || idx == 1) {
							costA += 4;
							costB += 3;
						} else {
							costA += tailBits + 2;
							costB += tailBits + 1;
						}
					} else {
						costA += 3;
						costB += 2;
					}
				} else if ((enc->m_samples_per_subband - 1) <= row) {
					costA += 2;
					++costB;
				} else {
					int32_t next = (int)floor(
					    (coeffs[((row + 1) * enc->m_numColumns) + col] + halfStep)
					    / (float)step);
					if (next) {
						costA += 2;
						++costB;
					} else {
						++costA;
						costB += 2;
						++row;
					}
				}
			}

			if (costA > costB) {
				costA = costB;
				++runFmt; /* switch to k12 / k23 / k34 / k44 orientation */
			}

			int32_t ternBits;
			if (absPeak != 4) {
				ternBits =
				    ((enc->m_samples_per_subband + 2) / 3) * ((absPeak * 2) + 3);
			} else {
				ternBits = (((enc->m_samples_per_subband < -1)
						 ? (enc->m_samples_per_subband + 2)
						 : (enc->m_samples_per_subband + 1))
					    >> 1)
					   * 7;
			}

			if (costA > ternBits) {
				costA = ternBits;
				runFmt = ternary_fmt[absPeak];
			}

			cost = costA;
			enc->m_pFormatIdPerColumn[col] = runFmt;
		} else if (minIdx >= -5 && maxIdx <= 5) {
			cost = (((enc->m_samples_per_subband < -1) ? (enc->m_samples_per_subband + 2)
								   : (enc->m_samples_per_subband + 1))
				>> 1)
			       * 7;
			enc->m_pFormatIdPerColumn[col] = 0x1D; /* t37 */
		} else {
			/* Fixed-width "linear": pick the bit width that spans the index range */
			int32_t mag = 0;
			if (minIdx < 0) {
				mag = ~minIdx;
			}

			if (maxIdx > 0 && ((uint32_t)(mag) < (uint32_t)(maxIdx))) {
				mag = maxIdx;
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
			cost = nbits * enc->m_samples_per_subband;
		}

		bits += cost;
		++colBase;
	}

	enc->m_quantPower = quantPower;
	enc->m_quantStep = step;
	return bits;
}

/*
 * Rate control: binary-search the quant step so the encoded block is the
 * largest that still fits within m_bitBudget (a smaller step is finer and
 * costs more bits).
 */
static void choose_quant_step(Encoder *enc)
{
	int32_t lo = 1;
	int32_t hi = 0x7FFF;

	do {
		const int32_t mid = (lo + hi) >> 1;
		int32_t bits = estimate_bits(enc, mid);
		if (enc->m_bitBudget < bits) {
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	} while (hi >= lo);

	if (enc->m_quantStep != lo) {
		estimate_bits(enc, lo);
	}
}

static void WriteBands(Encoder *enc)
{
	bits_write(&enc->m_bits, enc->m_quantPower, 4);
	bits_write(&enc->m_bits, enc->m_quantStep, 16);

	for (int i = 0; i < enc->m_numColumns; ++i) {
		const uint32_t formatId = enc->m_pFormatIdPerColumn[i];
		bits_write(&enc->m_bits, formatId, 5);
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
	for (int i = 0; i < enc->m_levels; ++i, overlap += overlap) {
		float *pDst = enc->m_levelSlots[i] - overlap;
		float *pSrc = pDst + enc->m_samplesPerBlock;
		memcpy(pDst, pSrc, overlap * sizeof(float));
	}
}

static void ProcessBlock(Encoder *enc)
{
	analyze(enc);

	if (enc->m_bandWriteEnabled != 0) {
		choose_quant_step(enc);

		WriteBands(enc);
	}

	shift_overlap(enc);

	enc->m_blockSamplesRemaining += enc->m_samplesPerBlock;
	enc->m_pCurrBlockData -= enc->m_samplesPerBlock;
}

static void EncodeSample(Encoder *enc)
{
	int32_t sample = 0;
	if (enc->m_finishedReading == 0) {
		sample = (*enc->m_reader)(enc->m_pReaderData);
		if (sample == (int32_t)ReadSampleEof) {
			enc->m_finishedReading = 1;
			return;
		}

		++enc->m_sampleCount;
	}

	*enc->m_pCurrBlockData++ = enc->m_volume * (float)sample;

	if (--enc->m_blockSamplesRemaining == 0) {
		ProcessBlock(enc);
	}
}

static void EncodeFlush(Encoder *enc)
{
	if (enc->m_samplesPerBlock == enc->m_blockSamplesRemaining) {
		// no data in the block
		return;
	}

	// Zero out the remaining data in the block
	while (enc->m_blockSamplesRemaining != 0) {
		*enc->m_pCurrBlockData++ = 0.0f;
		--enc->m_blockSamplesRemaining;
	}

	// Send it off for processing
	ProcessBlock(enc);

	/////////////
	// NOTE: The Interplay one doesn't do this ... but it should as there
	// may be bits left in the bit processor that should go out
	bits_flush(&enc->m_bits);
	/////////////
}

int32_t acm_encode(ReadSampleFunction *read, void *data, FILE *out, unsigned channels,
		   unsigned sample_rate, float volume, int levels, int samples_per_subband,
		   float comp_ratio)
{
	Encoder enc;
	memset(&enc, 0, sizeof(enc));

	ReadSample_init(&enc, read, data);
	bits_init(&enc.m_bits, out);

	enc.m_volume = volume;
	if (!SetupEncoder(&enc, 0xF, std_lo_filter, std_hi_filter, levels, samples_per_subband)) {
		DestroyEncoder(&enc);
		return 0;
	}

	enc.m_bitBudget = (int32_t)((float)enc.m_samplesPerBlock * comp_ratio * 16.0f);

	int32_t startPos = ftell(out);

	// Header
	bits_write(&enc.m_bits, 0x97, 8);
	bits_write(&enc.m_bits, 0x28, 8);
	bits_write(&enc.m_bits, 0x03, 8);

	// Version
	bits_write(&enc.m_bits, 1, 8);

	// Sample Count (Placeholder 32bits for now)
	bits_write(&enc.m_bits, 0, 8);
	bits_write(&enc.m_bits, 0, 8);
	bits_write(&enc.m_bits, 0, 8);
	bits_write(&enc.m_bits, 0, 8);

	// Number of channels
	bits_write(&enc.m_bits, channels, 16);

	// Sample Rate
	bits_write(&enc.m_bits, sample_rate, 16);

	// Levels
	bits_write(&enc.m_bits, levels, 4);

	// Samples per Sub-band (rows)
	bits_write(&enc.m_bits, samples_per_subband, 12);

	enc.m_bandWriteEnabled = 0;

	// Prime the analysis filters with lead-in samples before emitting output.
	int32_t primeCount = enc.m_primingLen;
	while (primeCount) {
		EncodeSample(&enc);
		--primeCount;
	}

	enc.m_bandWriteEnabled = 1;

	while (!enc.m_finishedReading) {
		EncodeSample(&enc);
	}

	// Flush the filters with the matching lead-out samples.
	primeCount = enc.m_primingLen;
	while (primeCount) {
		EncodeSample(&enc);
		--primeCount;
	}

	EncodeFlush(&enc);

	// Go back and write the Sample Count out proper
	fseek(out, startPos + 4, SEEK_SET);
	putc((enc.m_sampleCount >> 0) & 0xFF, out);
	putc((enc.m_sampleCount >> 8) & 0xFF, out);
	putc((enc.m_sampleCount >> 16) & 0xFF, out);
	putc((enc.m_sampleCount >> 24) & 0xFF, out);
	fseek(out, 0, SEEK_END);

	DestroyEncoder(&enc);
	return 1;
}
