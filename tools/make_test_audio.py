#!/usr/bin/env python3
"""Generate deterministic test audio for tests/bitexact_suite.sh.

Writes, into <outdir>:
  s16_44.wav   16-bit / 44.1 kHz stereo, 10 s of 1 kHz + 440 Hz sines
  s24_96.wav   24-bit / 96 kHz stereo, 12 s, the first 2 s silent (S24 detection path)
  s16_44.flac, s24_96.flac  the same samples in FLAC with VERBATIM subframes
No encoder needed: Fedora's ffmpeg-free ships no FLAC/ALAC/AAC encoder.
Usage: make_test_audio.py <outdir>
"""
import math, os, struct, sys, wave

def gen(path, rate, width, secs, silence, f1, f2):
    w = wave.open(path, "wb"); w.setnchannels(2); w.setsampwidth(width); w.setframerate(rate)
    full = (1 << (8 * width - 1)) - 1
    out = bytearray()
    for i in range(rate * secs):
        t = i / rate
        if t < silence:
            l = r = 0
        else:
            l = int(0.5 * full * math.sin(2 * math.pi * f1 * t))
            r = int(0.3 * full * math.sin(2 * math.pi * f2 * t))
        for v in (l, r):
            out += v.to_bytes(width, "little", signed=True)
    w.writeframes(bytes(out)); w.close()

def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x8005) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def utf8num(n):
    if n < 0x80: return bytes([n])
    if n < 0x800: return bytes([0xC0 | (n >> 6), 0x80 | (n & 0x3F)])
    if n < 0x10000: return bytes([0xE0 | (n >> 12), 0x80 | ((n >> 6) & 0x3F), 0x80 | (n & 0x3F)])
    return bytes([0xF0 | (n >> 18), 0x80 | ((n >> 12) & 0x3F), 0x80 | ((n >> 6) & 0x3F), 0x80 | (n & 0x3F)])

class Bits:
    def __init__(self): self.buf = bytearray(); self.acc = 0; self.n = 0
    def put(self, v, bits):
        self.acc = (self.acc << bits) | (v & ((1 << bits) - 1)); self.n += bits
        while self.n >= 8:
            self.n -= 8; self.buf.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1 if self.n else 0
    def pad(self):
        if self.n: self.put(0, 8 - self.n)

def wav2flac(src, dst):
    w = wave.open(src, "rb")
    ch, width, rate, nframes = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
    raw = w.readframes(nframes); w.close()
    bps = width * 8; BLOCK = 4096
    rates = {44100: 9, 48000: 10, 96000: 11, 88200: 8, 192000: 12}
    out = bytearray(b"fLaC")
    si = struct.pack(">HH", BLOCK, BLOCK) + b"\x00\x00\x00" + b"\x00\x00\x00"
    v = (rate << 44) | ((ch - 1) << 41) | ((bps - 1) << 36) | nframes
    si += v.to_bytes(8, "big") + b"\x00" * 16
    out += bytes([0x80]) + len(si).to_bytes(3, "big") + si   # last metadata block: STREAMINFO
    frame_no = 0; pos = 0
    while pos < nframes:
        n = min(BLOCK, nframes - pos)
        b = Bits()
        b.put(0x3FFE, 14); b.put(0, 1); b.put(0, 1)
        b.put(7 if n != BLOCK else 12, 4)
        b.put(rates.get(rate, 0), 4)
        b.put(ch - 1, 4)
        b.put({8: 1, 16: 4, 24: 6, 32: 0}[bps], 3); b.put(0, 1)
        hdr = bytearray(b.buf) + utf8num(frame_no)
        if n != BLOCK: hdr += struct.pack(">H", n - 1)
        hdr.append(crc8(hdr))
        fb = Bits()
        for c in range(ch):
            fb.put(0, 1); fb.put(1, 6); fb.put(0, 1)   # subframe: VERBATIM, no wasted bits
            for i in range(n):
                off = ((pos + i) * ch + c) * width
                fb.put(int.from_bytes(raw[off:off + width], "little", signed=True), bps)
        fb.pad()
        frame = bytes(hdr) + bytes(fb.buf)
        frame += struct.pack(">H", crc16(frame))
        out += frame
        pos += n; frame_no += 1
    open(dst, "wb").write(out)

if __name__ == "__main__":
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    a = os.path.join(outdir, "s16_44.wav"); b = os.path.join(outdir, "s24_96.wav")
    if not os.path.exists(a): gen(a, 44100, 2, 10, 0, 1000, 440)
    if not os.path.exists(b): gen(b, 96000, 3, 12, 2, 1000, 440)
    for src in (a, b):
        dst = src[:-4] + ".flac"
        if not os.path.exists(dst): wav2flac(src, dst)
    print("ok")
