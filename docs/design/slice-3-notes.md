# Slice 3 notes

What building [slice 3](../PRDs/Blackwing-Slice-3-PRD.md) found, milestone by milestone,
with the numbers. Written as it goes.

## Milestone 1, MP3

Nov 23, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- dr_mp3 is vendored from the dr_libs commit dr_flac came from, and only its low-level
  push decoder is used: it takes a buffer and decodes the frame at its front, so it never
  pulls and the decoder has nothing to undo after a wait. It is handed a fixed window, 32
  KB from where the last frame ended, which makes every decision it takes a function of
  the bytes and that place alone: every piece size and order gives the same samples.
- The reference is libmpg123 1.33.3 and the encoder libmp3lame 3.101, both already on
  the machine as shared libraries for its players, driven from `make_mp3.py` through
  ctypes. Nothing was installed.
- `DecoderInfo` gains `estimated`, for a length guessed from the size (no Xing, Info or
  VBRI header). The PRD asked for the length "marked as an estimate"; this is the mark.
- BR23 is done in `hifi`, not the player: the render thread drops `delay` frames at a
  source's start and everything from `frames - padding` on. Everything above counts the
  music's frames.

**Within a bit.** The ten good files, trimmed by their own delay and padding and rounded
to 16 bits, are within one of libmpg123 on every sample; 1128 of 361605 samples differ,
all by one. Each trimmed decode is exactly the 22050 (or 12000, 16000, 16537, 24000)
samples LAME was given. "Within a bit" is one, at 16 bits.

**Seeking does not use the Xing table to land.** The table says roughly where a time is
in the file (to a 256th of its size), never which frame begins there, and a frame's
number cannot be read from its bytes. Landing on the frame asked for, which the contract
promises and a gapless queue needs, means counting frames from a place whose count is
known. So the decoder keeps a mark every 8 frames as it walks (where the search starts,
and the two things dr_mp3 remembers there), and a seek walks forward from the nearest:
headers and side information only, with dr_mp3 keeping its bit reservoir, to two frames
before the target, which it decodes to fill the filter bank's history. A seek from a
mark (rather than from the decoder's own place) starts the reservoir's depth further back
(nine frames at MPEG 1's smallest, sixty-five at MPEG 2's). Every seek in the tests is
the straight decode, sample for sample. The table is used for what it is good for: how
much to ask for at once when a seek must read ahead. The PRD's "a wait per stretch" for a
cold file is what it costs, with or without the table.

**dr_mp3's sync wants four frames, not ten.** Its search takes a sync as a frame when
the next ten in the window agree. Junk in the middle of the broken-file corpus's CBR
file sat ten frames from the start, and the first half of the file was thrown away to
find ten in a row after the junk. Four agree as surely on any file not made to fool it,
and the file plays up to the junk.

**What a broken MP3 does**, from the corpus: a length that lies long ends `Damaged` with
the count, after every real frame; one that lies short decodes every frame and ends
`End` (`hifi` cuts at the claim). Junk between frames costs the frame before it and the
first after (silence: its reservoir was where the junk is) and ends `Damaged` with the
junk's size. A file cut short or inside a frame plays what it has. A front tag that says
it is longer than the file is refused. Six hundred spoiled files in pieces with seeks
kept every promise of the contract, under ASan and UBSan as well.

**Not done here**: an MP3's true length by reading it through (the background pass,
BL23), and the player's head read, which still reads a front tag whole before
`decoder_for` is asked (the decoder itself steps over it).
