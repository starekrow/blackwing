# Slice 2 notes

What building [slice 2](../PRDs/Blackwing-Slice-2-PRD.md) found, milestone by milestone,
with the numbers. Written as it goes.

## Milestone 1, seeking and the file in pieces

Nov 23, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- The span is `SparseSpan` (`hifi_codec/sparse.h`): a base address, a size, a piece
  size as a shift, and a byte per piece. A byte, not a bit: Korvid's atomics have no
  fetch-or, and a byte is set and cleared with a plain release store. A 2 GB file in
  256 KB pieces is an 8 KB map.
- `Wait` is a status of `decode` and of `seek`, and a `need` on `open`'s answer. A
  `decode` that waits may still return good frames before it: the frames before the
  missing piece are not thrown away to keep the rule simple.
- `seek` is its own call that may wait, rather than a target `decode` reaches, so that
  "it may fail, and then the decoder is where it was" can be said at the seek.
- `probe` still takes a `Span`, the file's first bytes. It needs no more.
- `hifi` and the tests hand the decoder `sparse_whole(bytes)`, one piece that is all
  there. Nothing heard changes; `hifi` treats a `Wait` from a whole file as damage, which
  cannot happen, until milestone 2 gives it something to wait for.

**How dr_flac is made to wait.** It reads by pulling through callbacks and cannot come
back from a short read, so the wrapper keeps it from making one. Before dr_flac starts a
FLAC frame, everything that frame can read from the cursor must be there: the largest
frame (STREAMINFO's, or the largest the format allows at its block size) and twice its
4 KB read-ahead. Otherwise `decode` returns `Wait` and dr_flac is not called. It is
asked for one frame across that line, then for the rest of that FLAC frame, so it never
starts a second without the check. Where the bytes cannot be known ahead (the headers, a
seek's search, a damaged frame's hunt for the next sync) the read callback refuses any
read that reaches a missing piece, all of it, so dr_flac caches nothing it was not
given. The call fails, and the wrapper puts dr_flac back at the frame it last returned,
by seeking from the first frame. In the tests only spoiled files take that path (84
times over two hundred spoiled files in pieces); good files never do.

dr_flac counts frames as it returns them, so after a dropped frame its count and the
file's frame numbers part. The wrapper keeps its own place as a frame of the file, from
the frame header, and seeks from the first frame whenever the two disagree.

**Exactness in pieces.** The nine test files (the eight, and the 24-bit 192 kHz one with
a seek table put in by `make_data.py --seektable`, since GStreamer's encoder writes
none) in four piece sizes (4 KB, 8 KB, 32 KB, 256 KB), four orders of delivery and two
sizes of read: 288 decodes, 2092 waits, every one the file's MD5. The span is reserved
address space and a piece is committed only when delivered, so a read of a piece not
there would fault rather than read zeros. None did.

**Seeks.** 3600 in the test files, each followed by up to 3000 frames compared with a
straight decode: all exact, by seek table, binary search, and brute force in a file
with its length zeroed. Past the end is refused with a reason and the decoder carries on
from where it was. Behind a 5 KB ID3v2 tag, the same seeks land the same. Into a
truncated file, past a lying length, into a spoiled frame:
fails or lands, and never plays anything that is not the file's own samples. Two
hundred spoiled files seeked at random in pieces keep to the contract.

**The real album** (ten tracks, 302 MB) through the heavy test: every track exact, now
over the callbacks rather than dr_flac's memory reader, at 1188 times real time (slice
1: about 1150). 160 seeks, sixteen a track at random, all exact; 215 us each on average,
the slowest 4 ms. These files have seek tables.

**Sanitizers.** `kormake test hifi_codec --sanitize address,undefined`: clean.

**Not done here, and noted:**

- Seek time by distance is for the close; the heavy test's average and worst are the
  first numbers.
- Without a seek table, a seek in a file that arrives in pieces waits once per probe of
  the binary search, and each wait starts the search again. Cheap in memory; on a cold
  file it is a read per probe, about ten for a large file.

## Milestone 2, reading in pieces

Nov 23, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- A piece is 256 KB (`PlayerConfig::piece_bytes`), and the cap 256 MB on bytes committed
  (`memory_bytes`). Eight reads are out at once at most, so 2 MB in flight.
- `hifi` gained the `Want` event, said once per range a decoder waits for, and
  `hifi_starve`, which ends a source whose bytes will not come. A waiting decoder is
  tried every 2 ms (no timed wait: KR10).
- The player reads, in order: wanted pieces; the head of the item playing and the next;
  then each on from where it is. The head is as much as `decoder_for` needs, which
  `decoder_head_bytes` (new in `hifi_codec`) says from the first piece: 64 bytes, or past
  a long ID3v2 tag.
- The stat goes through the I/O queue (BR24).
- `player.queue` rows gained `size` and `bytes_read`; `player.output` gained
  `bytes_held`; `player status` prints it.

**Giving a piece back safely.** A decoder may have seen a piece's byte set just before the
player clears it, and still be copying it. The player clears the byte, takes a fence
from `hifi` (a counter the render thread makes odd while it is in a step, where every
decoder call is), and decommits only once the fence has passed. A piece of an item
`hifi` no longer reads goes at once.

**Where the build departed from the plan:**

- `hifi` now opens the next source as soon as the current one has decoded its last
  frame, not when its last frames are written, so the next file's wants are said a
  lead's length ahead. And when the next source is still waiting for its first bytes as
  the current one's last frames come due, it holds them back while the lead lasts, so
  the join stays gapless. Without that, a next file whose head came late would end the
  stream and start a new one.
- The cap has to be larger than a decoder's window: FLAC's largest frame plus 8 KB
  (about 20 KB for CD audio). A 16 KB cap with 4 KB pieces deadlocks, the wanted range
  being larger than the cap. Real caps are thousands of times that.

**Tests.** `hifi`: two sources whose 4 KB pieces are handed over only when wanted play
exact and gapless; a source starved half way ends `Damaged`, exact up to there; one
starved before it opened fails. The player: two 56 KB files under a 40 KB cap in 4 KB
pieces, each starting before it is all read, exact and joined, never more than the cap
held; a file truncated on the disk while it is read ends damaged, "cut short". All three
modules clean under ThreadSanitizer, and under AddressSanitizer with UBSan.

**Through PipeWire**, on the built-in disk with no load: the first sound 0.36 s after
the player starts, and a skip heard 0.05 to 0.06 s after the command (two skips).

### The flash drive

`scripts/stress-play.sh` on the SanDisk Cruzer (USB 2, FAT32), slice 1's disk run: the
album evicted from the page cache, a 1 GB load file read and rewritten on the same drive
the whole time (buffered, dropping the cache: no O_DIRECT on FAT32), 90 seconds through
PipeWire. The script now stamps each line with the seconds since the player started,
and times each skip from the command to the track's first frame heard.

| run | the drive | first sound | skips | underruns |
|---|---|---|---|---|
| straight | 100% busy: 6 MB/s read, 2 MB/s written | 0.58 s | | 0 |
| a skip every 9 s | 100% busy: 8 MB/s read, 2 MB/s written | 0.66 s | 9 of 9 heard, 0.04 to 0.07 s after the command (0.05 on average) | 0 |

Slice 1 on the same drive under the same load: the first sound 24 seconds late (the
track read whole first), and at 9-second skips nothing heard at all. Now all ten tracks
the skipping run lands on play, each within a twentieth of a second, while the drive
stays saturated: a track starts on its first 256 KB, and the rest arrives well ahead of
the ear at 176 KB/s of CD audio. The exit criterion (a skip heard within two seconds,
every track played, no underrun) is met with a margin of forty.

Skips here are still slice 1's (a flush, then the next item added); the jump is
milestone 3.
