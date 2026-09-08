#!/usr/bin/env python3
"""Convert a stereo DSF file to DSDIFF (DFF), losslessly, for testing the
renderer's DFF parser against the same DSD content.

DSF stores DSD LSB-first in per-channel blocks of `blockSize` bytes
([L block][R block]...); DFF stores it MSB-first, byte-interleaved (L R L R…).
The conversion is bit-reversal of every byte plus re-interleaving.

Usage: dsf2dff.py <in.dsf> <out.dff>
"""
import struct, sys

REV = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))

def u64(b): return struct.unpack("<Q", b)[0]

def read_dsf(path):
    with open(path, "rb") as f:
        hdr = f.read(28)
        assert hdr[:4] == b"DSD ", "not a DSF file"
        fmt_off = 28
        f.seek(fmt_off)
        fmt = f.read(52)
        assert fmt[:4] == b"fmt "
        (fmt_size, ver, fid, ctype, channels, rate, bps, samples, block, _) = struct.unpack(
            "<QIIIIIIQII", fmt[4:52])
        assert channels == 2 and bps == 1, "stereo, 1 bit/sample expected"
        f.seek(fmt_off + fmt_size)
        data_hdr = f.read(12)
        assert data_hdr[:4] == b"data"
        data_size = u64(data_hdr[4:12]) - 12
        return rate, samples, block, f.read(data_size)

def write_dff(path, rate, data_interleaved):
    def chunk(tag, payload):
        return tag + struct.pack(">Q", len(payload)) + payload + (b"\x00" if len(payload) & 1 else b"")
    fver = chunk(b"FVER", struct.pack(">I", 0x01050000))
    chnl = chunk(b"CHNL", struct.pack(">H", 2) + b"SLFTSRGT")
    fs = chunk(b"FS  ", struct.pack(">I", rate))
    cmpr = chunk(b"CMPR", b"DSD " + bytes([14]) + b"not compressed" + b"\x00")
    prop = chunk(b"PROP", b"SND " + fs + chnl + cmpr)
    dsd = chunk(b"DSD ", data_interleaved)
    body = b"DSD " + fver + prop + dsd
    with open(path, "wb") as f:
        f.write(b"FRM8" + struct.pack(">Q", len(body)) + body)

if __name__ == "__main__":
    rate, samples, block, data = read_dsf(sys.argv[1])
    out = bytearray()
    per_ch_bytes = samples // 8
    pos = 0
    while pos + 2 * block <= len(data):
        left = data[pos:pos + block].translate(REV)
        right = data[pos + block:pos + 2 * block].translate(REV)
        out += bytes(b for pair in zip(left, right) for b in pair)
        pos += 2 * block
    out = bytes(out[:per_ch_bytes * 2])   # drop the zero padding of the last DSF block
    write_dff(sys.argv[2], rate, out)
    print(f"{sys.argv[2]}: {rate} Hz, {samples} samples/ch, {len(out)} bytes")
