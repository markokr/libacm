#!/usr/bin/env python3
"""
Dump raw (pre-transform) ACM block data.

This follows the bitstream parsing in src/decode.c up through
fill_block(), but skips juggle_block() -- the in-place wavelet-like
transform that decode_block() normally applies afterwards to turn the
per-column coefficients into the final sample block. What comes out
here is exactly what fill_block() wrote into acm->block: one signed
integer per (row, col) cell, still in "coefficient" space.

Usage:
    python3 dump_raw_block.py input.acm [output.txt]

With no output path, the data is written to stdout as text: one
"# block N pwr=.. val=.. rows=.. cols=.." header line per ACM block,
followed by `rows` lines of `cols` space-separated integers.
"""

import argparse
import sys

ACM_ID = 0x032897
WAVC_ID = 0x564157

MAP_1BIT = (-1, 1)
MAP_2BIT_NEAR = (-2, -1, 1, 2)
MAP_2BIT_FAR = (-3, -2, 2, 3)
MAP_3BIT = (-4, -3, -2, -1, 1, 2, 3, 4)


class AcmError(Exception):
    pass


class BitReader:
    """LSB-first bit reader, matching decode.c's get_bits (bits <= 31)."""

    def __init__(self, data: bytes):
        # decode.c pads the stream with a single zero byte once the real
        # file hits EOF; mirror that so a block ending exactly at EOF
        # still parses cleanly.
        self.data = data + b"\x00"
        self.pos = 0
        self.acc = 0
        self.nbits = 0

    def _fill(self) -> None:
        while self.nbits <= 24 and self.pos < len(self.data):
            self.acc |= self.data[self.pos] << self.nbits
            self.nbits += 8
            self.pos += 1

    def get_bits(self, n: int) -> int:
        self._fill()
        if self.nbits < n:
            raise EOFError("unexpected end of stream")
        val = self.acc & ((1 << n) - 1)
        self.acc >>= n
        self.nbits -= n
        return val


def read_header(br: BitReader) -> dict:
    tmp = br.get_bits(24)
    wavc_file = False

    if tmp == WAVC_ID:
        if br.get_bits(8) != ord("C"):
            raise AcmError("not an ACM file (bad WAVC id)")
        buf = [br.get_bits(16) for _ in range(12)]
        if buf[0] != 0x3156 or buf[1] != 0x302E:
            raise AcmError("not an ACM file (bad WAVC header)")
        if buf[6] != 28:
            raise AcmError("not an ACM file (missing WAVC magic 28)")
        wavc_file = True
        tmp = br.get_bits(24)

    if tmp != ACM_ID:
        raise AcmError("not an ACM file (bad id)")

    version = br.get_bits(8)
    if version != 1:
        raise AcmError(f"unsupported ACM version {version}")

    total_values = br.get_bits(16)
    total_values += br.get_bits(16) << 16

    channels = br.get_bits(16)
    if channels < 1 or channels > 2:
        raise AcmError(f"bad channel count {channels}")

    rate = br.get_bits(16)
    if rate < 4096:
        raise AcmError(f"bad sample rate {rate}")

    acm_level = br.get_bits(4)
    acm_rows = br.get_bits(12)
    if acm_rows == 0:
        raise AcmError("corrupt header (acm_rows == 0)")

    return {
        "wavc_file": wavc_file,
        "version": version,
        "total_values": total_values,
        "channels": channels,
        "rate": rate,
        "acm_level": acm_level,
        "acm_rows": acm_rows,
        "acm_cols": 1 << acm_level,
    }


# ---- fillers -----------------------------------------------------------
#
# Each ported 1:1 from the filler_t functions in src/decode.c. `out` is
# filled with the raw quantized index for every row of one column; the
# caller scales it by the block's `val` (this replaces acm->midbuf[idx]
# lookups, since midbuf[idx] == idx * val).


def f_zero(br, rows, fmt, out):
    for i in range(rows):
        out[i] = 0


def f_bad(br, rows, fmt, out):
    raise AcmError("corrupt block (reserved filler fmt)")


def f_binary(br, rows, fmt, out):
    middle = 1 << (fmt - 1)
    for i in range(rows):
        out[i] = br.get_bits(fmt) - middle


def f_peak1_zz(br: BitReader, rows: int, fmt: int, out: list[int]) -> None:
    i = 0
    while i < rows:
        if br.get_bits(1) == 0:
            out[i] = 0
            i += 1
            if i < rows:
                out[i] = 0
                i += 1
        elif br.get_bits(1) == 0:
            out[i] = 0
            i += 1
        else:
            out[i] = MAP_1BIT[br.get_bits(1)]
            i += 1


def f_peak1_z(br, rows, fmt, out):
    for i in range(rows):
        if br.get_bits(1) == 0:
            out[i] = 0
        else:
            out[i] = MAP_1BIT[br.get_bits(1)]


def f_peak2_zz(br, rows, fmt, out):
    i = 0
    while i < rows:
        if br.get_bits(1) == 0:
            out[i] = 0
            i += 1
            if i < rows:
                out[i] = 0
                i += 1
        elif br.get_bits(1) == 0:
            out[i] = 0
            i += 1
        else:
            out[i] = MAP_2BIT_NEAR[br.get_bits(2)]
            i += 1


def f_peak2_z(br, rows, fmt, out):
    for i in range(rows):
        if br.get_bits(1) == 0:
            out[i] = 0
        else:
            out[i] = MAP_2BIT_NEAR[br.get_bits(2)]


def f_peak3_zz(br, rows, fmt, out):
    i = 0
    while i < rows:
        if br.get_bits(1) == 0:
            out[i] = 0
            i += 1
            if i < rows:
                out[i] = 0
                i += 1
        elif br.get_bits(1) == 0:
            out[i] = 0
            i += 1
        elif br.get_bits(1) == 0:
            out[i] = MAP_1BIT[br.get_bits(1)]
            i += 1
        else:
            out[i] = MAP_2BIT_FAR[br.get_bits(2)]
            i += 1


def f_peak3_z(br, rows, fmt, out):
    for i in range(rows):
        if br.get_bits(1) == 0:
            out[i] = 0
        elif br.get_bits(1) == 0:
            out[i] = MAP_1BIT[br.get_bits(1)]
        else:
            out[i] = MAP_2BIT_FAR[br.get_bits(2)]


def f_peak4_zz(br, rows, fmt, out):
    i = 0
    while i < rows:
        if br.get_bits(1) == 0:
            out[i] = 0
            i += 1
            if i < rows:
                out[i] = 0
                i += 1
        elif br.get_bits(1) == 0:
            out[i] = 0
            i += 1
        else:
            out[i] = MAP_3BIT[br.get_bits(3)]
            i += 1


def f_peak4_z(br, rows, fmt, out):
    for i in range(rows):
        if br.get_bits(1) == 0:
            out[i] = 0
        else:
            out[i] = MAP_3BIT[br.get_bits(3)]


def f_peak1_base3(br, rows, fmt, out):
    i = 0
    while i < rows:
        b = br.get_bits(5)
        if b >= 3 * 3 * 3:
            raise AcmError("corrupt block (f_t15 out of range)")
        tmp = b // 3
        out[i] = b % 3 - 1
        i += 1
        if i < rows:
            out[i] = tmp % 3 - 1
            i += 1
            if i < rows:
                out[i] = tmp // 3 - 1
                i += 1


def f_peak2_base5(br, rows, fmt, out):
    i = 0
    while i < rows:
        b = br.get_bits(7)
        if b >= 5 * 5 * 5:
            raise AcmError("corrupt block (f_t27 out of range)")
        tmp = b // 5
        out[i] = b % 5 - 2
        i += 1
        if i < rows:
            out[i] = tmp % 5 - 2
            i += 1
            if i < rows:
                out[i] = tmp // 5 - 2
                i += 1


def f_peak5_base11(br, rows, fmt, out):
    i = 0
    while i < rows:
        b = br.get_bits(7)
        if b >= 11 * 11:
            raise AcmError("corrupt block (f_t37 out of range)")
        out[i] = b % 11 - 5
        i += 1
        if i < rows:
            out[i] = b % 11 - 5
            i += 1


FILLER_LIST = (
    f_zero, f_bad, f_bad, f_binary,                     # 0..3
    f_binary, f_binary, f_binary, f_binary,             # 4..7
    f_binary, f_binary, f_binary, f_binary,             # 8..11
    f_binary, f_binary, f_binary, f_binary,             # 12..15
    f_binary, f_peak1_zz, f_peak1_z, f_peak1_base3,     # 16..19
    f_peak2_zz, f_peak2_z, f_peak2_base5, f_peak3_zz,   # 20..23
    f_peak3_z, f_bad, f_peak4_zz, f_peak4_z,            # 24..27
    f_bad, f_peak5_base11, f_bad, f_bad,                # 28..31
)


def decode_block_raw(br: BitReader, info: dict) -> tuple:
    """Read one ACM block and return its raw (pre-juggle) coefficients.

    Mirrors decode_block() in src/decode.c up to (and including)
    fill_block(), but never calls juggle_block() -- so the returned
    values are exactly what the bitstream's fillers produced, laid out
    row-major like acm->block (index = row * acm_cols + col).
    """
    pwr = br.get_bits(4)
    step = br.get_bits(16)

    rows = info["acm_rows"]
    cols = info["acm_cols"]
    block = [0] * (rows * cols)
    row_buf = [0] * rows
    fillers = []

    for col in range(cols):
        ind = br.get_bits(5)
        fillers.append(ind)
        FILLER_LIST[ind](br, rows, ind, row_buf)
        for row in range(rows):
            block[row * cols + col] = row_buf[row] * step

    return block, pwr, step, fillers


def dump(acm_path: str, out_fh, max_blocks: int | None = None) -> int:
    with open(acm_path, "rb") as f:
        data = f.read()

    br = BitReader(data)
    info = read_header(br)

    print(
        f"# {acm_path}: version={info['version']} channels={info['channels']} "
        f"rate={info['rate']} acm_level={info['acm_level']} "
        f"acm_rows={info['acm_rows']} acm_cols={info['acm_cols']} "
        f"total_values={info['total_values']} wavc={info['wavc_file']}",
        file=out_fh,
    )

    rows = info["acm_rows"]
    cols = info["acm_cols"]
    block_no = 0
    while max_blocks is None or block_no < max_blocks:
        try:
            block, pwr, val, fillers = decode_block_raw(br, info)
        except EOFError:
            break

        print(f"# block {block_no} pwr={pwr} val={val} rows={rows} cols={cols}", file=out_fh)
        for c in range(cols):
            vals = []
            for r in range(rows):
                v = block[r * cols + c]
                vals.append(str(v))
            print(f"col={c} fmt={fillers[c]}: {' '.join(vals)}")
        #for r in range(rows):
        #    row = block[r * cols:(r + 1) * cols]
        #    print(" ".join(str(v) for v in row), file=out_fh)

        block_no += 1

    return block_no


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", help="input .acm file")
    ap.add_argument("output", nargs="?", help="output text file (default: stdout)")
    ap.add_argument("--max-blocks", type=int, default=None, help="stop after N blocks")
    args = ap.parse_args()

    out_fh = open(args.output, "w") if args.output else sys.stdout
    try:
        n = dump(args.input, out_fh, args.max_blocks)
    except AcmError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        if out_fh is not sys.stdout:
            out_fh.close()

    print(f"# dumped {n} blocks", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
