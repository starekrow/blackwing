#!/usr/bin/env python3
"""Makes the FLAC files hifi_codec's tests read. Run from this directory; needs GStreamer's
gst-launch-1.0 with flacenc (gstreamer1.0-plugins-good). The files are committed, so this
runs only when the set changes.

Good files: deterministic PCM (a few sines and low-level noise, so the encoder uses its
predictors and its residual coder), encoded by libFLAC. Each file's STREAMINFO MD5 is
checked here against the MD5 of the PCM it was made from, so the tests' oracle (FLAC's own
MD5) is itself checked once against something that is not a FLAC decoder.

Broken files (broken/): made from the good ones by cutting and patching bytes.

A file with a seek table (GStreamer's flacenc writes none, so seeking in the good files is
searching): one good file with a SEEKTABLE block put in, a point at every frame. Made
from the committed file by itself: `make_data.py --seektable` needs no encoder.

A longer file, four seconds at 8 kHz, for what needs a track to last past a few seconds
(the player's previous): `make_data.py --long`, which encodes only it.
"""
import hashlib, math, os, struct, subprocess, sys, tempfile

GOOD = [  # bits, channels, rate, seconds
    (16, 1, 44100, 0.50), (16, 2, 44100, 0.50), (16, 2, 48000, 0.50), (24, 2, 48000, 0.50),
    (24, 1, 96000, 0.25), (24, 2, 96000, 0.25), (16, 2, 192000, 0.25), (24, 2, 192000, 0.25),
]

def pcm(bits, channels, rate, seconds):
    n = int(rate * seconds)
    full = (1 << (bits - 1)) - 1
    seed = 0x2545F491 ^ (bits * 7919 + channels * 104729 + rate)
    out = bytearray()
    for i in range(n):
        t = i / rate
        for c in range(channels):
            seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
            noise = ((seed >> 16) & 0xFF) - 128                      # about 8 bits of noise
            v = 0.45 * math.sin(2 * math.pi * 440.0 * t + c * 0.3)
            v += 0.25 * math.sin(2 * math.pi * 1337.0 * t)
            v += 0.10 * math.sin(2 * math.pi * (5000.0 + 900.0 * c) * t)
            s = max(-full - 1, min(full, int(v * full) + noise))
            out += s.to_bytes(bits // 8, "little", signed=True)
    return bytes(out), n

def encode(raw, bits, channels, rate, path):
    with tempfile.NamedTemporaryFile(suffix=".raw", delete=False) as f:
        f.write(raw)
    fmt = {16: "s16le", 24: "s24le"}[bits]
    cmd = ["gst-launch-1.0", "-q", "filesrc", "location=" + f.name, "!", "rawaudioparse", "format=pcm",
           "pcm-format=" + fmt, "sample-rate=%d" % rate, "num-channels=%d" % channels, "!",
           "flacenc", "quality=6", "!", "filesink", "location=" + path]
    subprocess.run(cmd, check=True)
    os.unlink(f.name)

def streaminfo(data):
    assert data[:4] == b"fLaC" and (data[4] & 0x7F) == 0, "STREAMINFO is not first"
    si = data[8:8 + 34]
    packed = int.from_bytes(si[10:18], "big")
    return dict(rate=packed >> 44, channels=((packed >> 41) & 7) + 1, bits=((packed >> 36) & 31) + 1,
                total=packed & ((1 << 36) - 1), md5=si[18:34])

def crc8(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x07) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

def audio_start(data):
    o = 4
    while True:
        last, n = data[o] & 0x80, int.from_bytes(data[o + 1:o + 4], "big")
        o += 4 + n
        if last:
            return o

def frames(data, block):
    """(offset, first sample) of every frame of a fixed-block-size stream, found by its
    sync code, its CRC-8 and its frame number counting up from 0."""
    out, o, want = [], audio_start(data), 0
    while o + 16 <= len(data):
        if data[o] != 0xFF or data[o + 1] != 0xF8:
            o += 1
            continue
        bs, rate = data[o + 2] >> 4, data[o + 2] & 15
        k = o + 4
        lead = data[k]
        extra = 0 if lead < 0x80 else (7 - (lead ^ 0xFF).bit_length())
        num = lead & (0x7F >> extra) if extra else lead
        for j in range(extra):
            num = (num << 6) | (data[k + 1 + j] & 0x3F)
        k += 1 + extra
        k += {6: 1, 7: 2}.get(bs, 0) + {12: 1, 13: 2, 14: 2}.get(rate, 0)
        if num == want and crc8(data[o:k]) == data[k]:
            out.append((o, num * block))
            want += 1
            o = k
        else:
            o += 1
    return out

def with_seektable(data):
    si = streaminfo(data)
    block = int.from_bytes(data[8:10], "big")
    points = b""
    found = frames(data, block)
    for i, (off, sample) in enumerate(found):
        n = (found[i + 1][1] if i + 1 < len(found) else si["total"]) - sample
        points += sample.to_bytes(8, "big") + (off - audio_start(data)).to_bytes(8, "big") + n.to_bytes(2, "big")
    assert found and found[-1][1] < si["total"] <= found[-1][1] + block
    table = bytes([3]) + len(points).to_bytes(3, "big") + points   # SEEKTABLE, not the last block
    return data[:42] + table + data[42:], len(found)

def seektable():
    src = "tone_24_2ch_192000.flac"
    out, n = with_seektable(open(src, "rb").read())
    name = "tone_24_2ch_192000_seektable.flac"
    open(name, "wb").write(out)
    print("%-28s %7d bytes, %d seek points" % (name, len(out), n))

def long_file():
    bits, channels, rate, seconds = 16, 2, 8000, 4.0
    raw, n = pcm(bits, channels, rate, seconds)
    name = "tone_16_2ch_8000_long.flac"
    encode(raw, bits, channels, rate, name)
    si = streaminfo(open(name, "rb").read())
    assert (si["rate"], si["channels"], si["bits"], si["total"]) == (rate, channels, bits, n), (name, si)
    assert si["md5"] == hashlib.md5(raw).digest(), name + ": STREAMINFO MD5 is not the PCM's"
    print("%-28s %7d bytes, %6d frames, md5 %s" % (name, os.path.getsize(name), n, si["md5"].hex()))

def main():
    names = []
    for bits, channels, rate, seconds in GOOD:
        raw, n = pcm(bits, channels, rate, seconds)
        name = "tone_%d_%dch_%d.flac" % (bits, channels, rate)
        encode(raw, bits, channels, rate, name)
        si = streaminfo(open(name, "rb").read())
        assert (si["rate"], si["channels"], si["bits"], si["total"]) == (rate, channels, bits, n), (name, si)
        assert si["md5"] == hashlib.md5(raw).digest(), name + ": STREAMINFO MD5 is not the PCM's"
        names.append(name)
        print("%-28s %7d bytes, %6d frames, md5 %s" % (name, os.path.getsize(name), n, si["md5"].hex()))

    good = open("tone_16_2ch_44100.flac", "rb").read()
    os.makedirs("broken", exist_ok=True)
    def put(name, data):
        open(os.path.join("broken", name), "wb").write(data)
        print("broken/%-21s %7d bytes" % (name, len(data)))
    put("truncated.flac", good[: len(good) * 6 // 10])                 # cut inside a frame
    put("not_flac.flac", b"RIFF\x24\x08\x00\x00WAVEfmt " + bytes(range(256)) * 4)
    put("empty.flac", b"")
    put("magic_only.flac", b"fLaC")
    put("header_only.flac", good[:42])                                  # STREAMINFO and nothing after
    lying = bytearray(good)                                             # twice the frames it has
    packed = int.from_bytes(lying[18:26], "big")
    total = packed & ((1 << 36) - 1)
    lying[18:26] = ((packed & ~((1 << 36) - 1)) | (total * 2)).to_bytes(8, "big")
    put("lying_length.flac", bytes(lying))
    bad = bytearray(good)                                               # one frame's bytes spoiled
    mid = len(bad) // 2
    for k in range(24):
        bad[mid + k] ^= 0x5A
    put("bad_frame.flac", bytes(bad))
    wild = bytearray(good)                                              # a header that claims 8 channels of 32 bits
    packed = int.from_bytes(wild[18:26], "big")
    packed |= (7 << 41) | (31 << 36)
    wild[18:26] = packed.to_bytes(8, "big")
    put("lying_format.flac", bytes(wild))
    seektable()
    long_file()

if __name__ == "__main__":
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    if sys.argv[1:] == ["--seektable"]: seektable()
    elif sys.argv[1:] == ["--long"]: long_file()
    else: main()
