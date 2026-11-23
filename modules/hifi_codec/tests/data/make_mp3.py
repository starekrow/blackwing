#!/usr/bin/env python3
"""Makes the MP3 files hifi_codec's tests read, and what a reference decoder made of each.
Run from this directory. Needs libmp3lame and libmpg123 as shared libraries (on Debian and
Ubuntu, libmp3lame0 and libmpg123-0, which most desktops have for their players); they are
driven through ctypes, so no command-line encoder or decoder is installed. The files are
committed, so this runs only when the set changes.

Good files (mp3/): deterministic PCM (the FLAC files' sines and noise, at 16 bits), encoded
by LAME: CBR and VBR, MPEG 1, 2 and 2.5, mono and stereo, with and without the Info tag,
free format, with CRCs. Beside each, <name>.ref: what libmpg123 decoded from it, 16-bit
little-endian interleaved, with its gapless trimming (which needs the LAME tag) on. That
is the oracle for "within a bit of a reference decoder".

Made from those by this script, with no encoder: the same audio behind a front tag of
200 KB of art and before an APEv2 and an ID3v1 tag (whose decode must equal the plain
file's), and a VBRI header in place of the Info frame.

Broken files (mp3/broken/): made from the good ones by cutting and patching bytes.
"""
import ctypes, math, os, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "mp3")

lame = ctypes.CDLL("libmp3lame.so.0")
mpg = ctypes.CDLL("libmpg123.so.0")
lame.lame_init.restype = ctypes.c_void_p
for fn in ("lame_set_num_channels", "lame_set_in_samplerate", "lame_set_out_samplerate", "lame_set_brate",
           "lame_set_VBR", "lame_set_VBR_q", "lame_set_mode", "lame_set_quality", "lame_set_bWriteVbrTag",
           "lame_set_free_format", "lame_set_error_protection", "lame_set_write_id3tag_automatic",
           "lame_init_params", "lame_get_encoder_delay", "lame_get_encoder_padding", "lame_close"):
    getattr(lame, fn).argtypes = [ctypes.c_void_p] + ([ctypes.c_int] if fn.startswith("lame_set") else [])
lame.lame_encode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
lame.lame_encode_buffer_interleaved.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
lame.lame_encode_flush.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
lame.lame_get_lametag_frame.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
lame.lame_get_lametag_frame.restype = ctypes.c_size_t
lame.get_lame_version.restype = ctypes.c_char_p

mpg.mpg123_new.restype = ctypes.c_void_p
mpg.mpg123_new.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]
mpg.mpg123_param2.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_double]
mpg.mpg123_format_none.argtypes = [ctypes.c_void_p]
mpg.mpg123_format.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_int, ctypes.c_int]
mpg.mpg123_open.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
mpg.mpg123_read.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
mpg.mpg123_close.argtypes = [ctypes.c_void_p]
mpg.mpg123_delete.argtypes = [ctypes.c_void_p]
mpg.mpg123_distversion.restype = ctypes.c_char_p
mpg.mpg123_distversion.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]

MPG123_ADD_FLAGS, MPG123_GAPLESS, MPG123_QUIET = 2, 0x40, 0x20
MPG123_ENC_SIGNED_16 = 0xD0
MPG123_DONE, MPG123_NEW_FORMAT = -12, -11
VBR_OFF, VBR_DEFAULT = 0, 4
STEREO, JOINT, MONO = 0, 1, 3

GOOD = [  # name, rate, channels, seconds, settings
    ("cbr128_44100_2ch", 44100, 2, 0.50, dict(brate=128)),
    ("vbr_44100_2ch",    44100, 2, 0.50, dict(vbr=2)),
    ("vbr_48000_2ch",    48000, 2, 0.50, dict(vbr=4)),
    ("cbr64_22050_1ch",  22050, 1, 0.75, dict(brate=64)),
    ("vbr_24000_2ch",    24000, 2, 0.50, dict(vbr=5)),
    ("cbr32_8000_1ch",    8000, 1, 1.50, dict(brate=32)),
    ("cbr320_32000_2ch", 32000, 2, 0.50, dict(brate=320, mode=STEREO)),
    ("notag_44100_2ch",  44100, 2, 0.50, dict(brate=128, tag=False)),
    ("free_44100_2ch",   44100, 2, 0.50, dict(brate=200, free=True)),
    ("crc_44100_2ch",    44100, 2, 0.50, dict(brate=160, crc=True)),
]
LONG = ("long_44100_2ch", 44100, 2, 12.0, dict(vbr=4))  # for seeking: no reference kept

def pcm(rate, channels, seconds):
    """16-bit samples as a list of lists (one per channel): the FLAC set's tone."""
    n = int(rate * seconds)
    full = 32767
    seed = 0x2545F491 ^ (16 * 7919 + channels * 104729 + rate)
    out = [[0] * n for _ in range(channels)]
    for i in range(n):
        t = i / rate
        for c in range(channels):
            seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
            noise = (((seed >> 16) & 0xFF) - 128) >> 3
            v = 0.45 * math.sin(2 * math.pi * 440.0 * t + c * 0.3)
            v += 0.25 * math.sin(2 * math.pi * 1337.0 * t)
            v += 0.10 * math.sin(2 * math.pi * (min(5000.0, rate * 0.3) + 300.0 * c) * t)
            out[c][i] = max(-full - 1, min(full, int(v * full * 0.8) + noise))
    return out, n

def encode(rate, channels, seconds, s):
    chans, n = pcm(rate, channels, seconds)
    gf = lame.lame_init()
    lame.lame_set_num_channels(gf, channels)
    lame.lame_set_in_samplerate(gf, rate)
    lame.lame_set_out_samplerate(gf, rate)
    lame.lame_set_write_id3tag_automatic(gf, 0)
    lame.lame_set_quality(gf, 2)
    lame.lame_set_mode(gf, MONO if channels == 1 else s.get("mode", JOINT))
    if "vbr" in s:
        lame.lame_set_VBR(gf, VBR_DEFAULT)
        lame.lame_set_VBR_q(gf, s["vbr"])
    else:
        lame.lame_set_VBR(gf, VBR_OFF)
        lame.lame_set_brate(gf, s["brate"])
    lame.lame_set_bWriteVbrTag(gf, 0 if s.get("tag", True) is False or s.get("free") else 1)
    if s.get("free"): lame.lame_set_free_format(gf, 1)
    if s.get("crc"): lame.lame_set_error_protection(gf, 1)
    assert lame.lame_init_params(gf) >= 0, "lame_init_params"
    cap = int(1.25 * n + 7200) * 2
    buf = ctypes.create_string_buffer(cap)
    data = bytearray()
    step = 4608
    for at in range(0, n, step):
        k = min(step, n - at)
        if channels == 1:
            l = (ctypes.c_short * k)(*chans[0][at:at + k])
            got = lame.lame_encode_buffer(gf, l, l, k, buf, cap)
        else:
            inter = []
            for i in range(at, at + k): inter += [chans[0][i], chans[1][i]]
            got = lame.lame_encode_buffer_interleaved(gf, (ctypes.c_short * (2 * k))(*inter), k, buf, cap)
        assert got >= 0, "lame_encode_buffer"
        data += buf.raw[:got]
    got = lame.lame_encode_flush(gf, buf, cap)
    data += buf.raw[:got]
    tag = ctypes.create_string_buffer(8192)
    tl = lame.lame_get_lametag_frame(gf, tag, 8192)
    if tl and s.get("tag", True) and not s.get("free"):
        data[:tl] = tag.raw[:tl]  # the Info frame, over the placeholder LAME left at the front
    delay, padding = lame.lame_get_encoder_delay(gf), lame.lame_get_encoder_padding(gf)
    lame.lame_close(gf)
    return bytes(data), n, delay, padding

def reference(data, rate, channels):
    """What libmpg123 decodes from `data`, gapless, as 16-bit little-endian bytes."""
    err = ctypes.c_int(0)
    h = mpg.mpg123_new(None, ctypes.byref(err))
    mpg.mpg123_param2(h, MPG123_ADD_FLAGS, MPG123_GAPLESS | MPG123_QUIET, 0.0)
    mpg.mpg123_format_none(h)
    mpg.mpg123_format(h, rate, channels, MPG123_ENC_SIGNED_16)
    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as f:
        f.write(data)
    assert mpg.mpg123_open(h, f.name.encode()) == 0, "mpg123_open"
    out = bytearray()
    buf = ctypes.create_string_buffer(65536)
    done = ctypes.c_size_t(0)
    while True:
        r = mpg.mpg123_read(h, buf, 65536, ctypes.byref(done))
        out += buf.raw[:done.value]
        if r == MPG123_DONE: break
        assert r in (0, MPG123_NEW_FORMAT), "mpg123_read: %d" % r
    mpg.mpg123_close(h)
    mpg.mpg123_delete(h)
    os.unlink(f.name)
    return bytes(out)

def syncsafe(n):
    return bytes([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F])

def id3v2_with_art(art_bytes):
    """An ID3v2.3 tag: a title, and an APIC frame of `art_bytes` bytes of not-a-JPEG."""
    def frame(fid, body): return fid + struct.pack(">I", len(body)) + b"\0\0" + body
    title = frame(b"TIT2", b"\0Tone")
    seed, art = 12345, bytearray()
    for _ in range(art_bytes):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        art.append((seed >> 16) & 0xFF)
    apic = frame(b"APIC", b"\0image/jpeg\0\x03\0" + bytes(art))
    body = title + apic + b"\0" * 1024  # and padding
    return b"ID3\x03\0\0" + syncsafe(len(body)) + body

def apev2(items):
    body = b"".join(struct.pack("<II", len(v), 0) + k + b"\0" + v for k, v in items)
    size = len(body) + 32
    head = b"APETAGEX" + struct.pack("<IIII", 2000, size, len(items), 0xA0000000) + b"\0" * 8
    foot = b"APETAGEX" + struct.pack("<IIII", 2000, size, len(items), 0x80000000) + b"\0" * 8
    return head + body + foot

def id3v1(title):
    return b"TAG" + title.ljust(30, b"\0") + b"Artist".ljust(30, b"\0") + b"Album".ljust(30, b"\0") + b"2026" + b"\0" * 30 + b"\xff"

# ---- MPEG frame helpers, for the made and broken files --------------------------------

BITRATES = {  # (mpeg1?) -> layer III table
    True:  [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    False: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}

def header_at(data, o):
    if o + 4 > len(data) or data[o] != 0xFF or (data[o + 1] & 0xE0) != 0xE0: return None
    ver, layer = (data[o + 1] >> 3) & 3, (data[o + 1] >> 1) & 3
    bri, sri, pad = data[o + 2] >> 4, (data[o + 2] >> 2) & 3, (data[o + 2] >> 1) & 1
    if ver == 1 or layer != 1 or bri == 15 or bri == 0 or sri == 3: return None
    rate = RATES[ver][sri]
    br = BITRATES[ver == 3][bri] * 1000
    size = (144 if ver == 3 else 72) * br // rate + pad
    return dict(size=size, mono=(data[o + 3] >> 6) == 3, mpeg1=ver == 3)

def frames(data, start):
    """Offsets of the frames from `start`, walking headers."""
    out, o = [], start
    while True:
        h = header_at(data, o)
        if not h: return out
        out.append(o)
        o += h["size"]

def info_frame_offset(data):
    h = header_at(data, 0)
    side = (32 if not h["mono"] else 17) if h["mpeg1"] else (17 if not h["mono"] else 9)
    return 4 + side

def vbri_from_info(data, nframes):
    """The file with its Info frame replaced by a VBRI frame of the same size, no TOC."""
    h = header_at(data, 0)
    f = bytearray(data[:h["size"]])
    for i in range(4, len(f)): f[i] = 0
    body = b"VBRI" + struct.pack(">HHHII", 1, 0, 75, len(data), nframes) + struct.pack(">HHHH", 0, 1, 2, 1)
    f[36:36 + len(body)] = body
    return bytes(f) + data[h["size"]:]

def write(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)

def main():
    os.makedirs(os.path.join(OUT, "broken"), exist_ok=True)
    print("lame", lame.lame_get_lame_version() if hasattr(lame, "lame_get_lame_version") else lame.get_lame_version().decode(),
          "/ mpg123", mpg.mpg123_distversion(None, None, None).decode())
    mpg.mpg123_init()
    made = {}
    for name, rate, ch, secs, s in GOOD + [LONG]:
        data, n, delay, padding = encode(rate, ch, secs, s)
        write(name + ".mp3", data)
        made[name] = data
        if name != LONG[0]:
            ref = reference(data, rate, ch)
            write(name + ".mp3.ref", ref)
            print("%-18s %6d bytes, %6d samples in, encoder delay %4d padding %4d, reference %6d frames"
                  % (name, len(data), n, delay, padding, len(ref) // (2 * ch)))
        else:
            print("%-18s %6d bytes, %6d samples in" % (name, len(data), n))

    # The same audio behind 200 KB of art, and before an APEv2 and an ID3v1 tag.
    cbr = made["cbr128_44100_2ch"]
    write("tagged_44100_2ch.mp3", id3v2_with_art(200 << 10) + cbr +
          apev2([(b"Title", b"Tone"), (b"Artist", b"Blackwing")]) + id3v1(b"Tone"))
    # The VBR file with a VBRI header in place of its Info frame.
    vbr = made["vbr_44100_2ch"]
    n = len(frames(vbr, 0)) - 1
    write("vbri_44100_2ch.mp3", vbri_from_info(vbr, n))

    # ---- broken ----
    b = lambda name, data: write("broken/" + name, data)
    fl = frames(cbr, 0)
    xo = info_frame_offset(cbr)
    assert cbr[xo:xo + 4] == b"Info"
    n_audio = len(fl) - 1
    lying = bytearray(cbr); lying[xo + 8:xo + 12] = struct.pack(">I", n_audio * 2)
    b("lying_long.mp3", bytes(lying))                         # says it has twice the frames
    lying = bytearray(cbr); lying[xo + 8:xo + 12] = struct.pack(">I", n_audio // 2)
    b("lying_short.mp3", bytes(lying))                        # says it has half
    lame_at = xo + 8 + 4 + 4 + 100 + 4
    assert cbr[lame_at:lame_at + 4] == b"LAME", cbr[lame_at:lame_at + 4]
    lying = bytearray(cbr); lying[lame_at + 21:lame_at + 24] = b"\xff\xff\xff"
    b("lying_gapless.mp3", bytes(lying))                       # delay and padding of 4095 each
    mid = fl[len(fl) // 2]
    seed, junk = 99, bytearray()
    for _ in range(3000):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        junk.append((seed >> 16) & 0xFF)
    b("junk_between.mp3", cbr[:mid] + bytes(junk) + cbr[mid:])
    b("truncated.mp3", cbr[:int(len(cbr) * 0.6)])
    b("cut_in_frame.mp3", cbr[:fl[5] + 100])
    notag = made["notag_44100_2ch"]
    nf = frames(notag, 0)
    b("one_frame.mp3", notag[nf[3]:nf[4]])
    b("info_only.mp3", cbr[:fl[1]])                            # the Info frame and nothing after it
    b("empty.mp3", b"")
    b("id3_only.mp3", id3v2_with_art(1000))
    big = bytearray(id3v2_with_art(1000)); big[6:10] = syncsafe(200 << 20)
    b("id3_size_lies.mp3", bytes(big) + cbr)                    # a front tag that says it is 200 MB
    b("not_mp3.mp3", open(os.path.join(HERE, "tone_16_2ch_44100.flac"), "rb").read())
    b("text.mp3", b"This is not an MP3 file, whatever its name says.\n" * 20)
    b("zeros_then_frames.mp3", b"\0" * 5000 + notag)          # junk at the front, no tag to say how much
    print("broken: done")

if __name__ == "__main__":
    main()
