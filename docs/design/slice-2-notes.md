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

## Milestone 3, the jump

Nov 23, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- `hifi` keeps two slots, each a ring and its counters (`write`, `end`, `read`, `base`).
  A render writes one; the callback hears one. `hifi_jump` starts a new render in the
  slot not heard and posts a join request (its epoch); the callback fades the heard ring
  out over 10 ms, takes the request with a compare-exchange once the new ring has 50 ms
  (or all it will have), sets that slot's `base` to the stream frame it is at, and fades
  it in. A newer jump takes a waiting request back with a compare-exchange of its own:
  whichever thread wins, the other knows which slot is free.
- `hifi_flush` is a jump to nothing, ramped over slice 1's 5 ms rather than faded, and
  what follows starts clean. Slice 1's `skip_to` and `end_frame` are gone.
- A source carries where it starts (`start`) and whether it was being heard (`seek`):
  its first mark is then `Seeked`, with the frame, and `position` counts from it.
- The new render's marks are made before anyone knows where its frame 0 will fall in the
  stream. They carry the slot's frame, and `hifi_poll` places them once the callback's
  `joined` names their epoch. (First built holding them on the render thread, which
  lost them when that thread was asleep by the time the join happened.)
- The player: `PlayerSeek`, `PlayerSeekBy`, `PlayerPrevious`, `PlayerJump`, `PlayerNext`
  all move its place and call `hifi_jump` with what is ready from there; what is not
  ready follows as it is. An item can be lent twice (the old render letting go while the
  new one has it), so the player counts lends. `player.transport` gained `place`,
  `seek_item` and `seek_frame`. The CLI gained `player previous`, `jump`, `seek`,
  `seekby` (BR26's part for these).
- `player.output`'s `lead_frames` is now the lead that will be heard: a jump's new render
  while it waits to be joined.

**Found by the tests:**

- A race: if the callback joined the new render just before the render thread looked,
  the render thread could hold the device and sleep while the next source was still
  waiting for its first bytes, and nothing woke it. A source waiting for bytes now
  always means "look again in 2 ms".
- Two seek-bys in one tick: the first took the row out of play, so the second saw it as
  not playing and made it a fresh start. "Is this a seek" is now "is this row the one
  heard, or is a seek to it on its way".
- Going back to a row that was done: its file had been let go of, but it was marked as
  already looked at, so it was never read again. A row that comes back returns to the
  state its pieces allow.

**Tests.** `hifi`: a seek lands on its frame, exact from the end of the 10 ms fade in,
the old audio faded at once and no silence on a warm file, no step at either fade larger
than the music's own, `Seeked` (not `Started`) with the frame, the position the target
plus every frame heard since; a skip to a file whose pieces are slow is silence and no
source until they come, then the file from its start; a jump to a file that never gets
its bytes, then another, plays only the second; a jump while paused waits for play. The
player: the same seek through events, two seek-bys in a tick adding up, previous past
and within three seconds, a jump to row 3. Slice 1's next test now expects a jump's
fades (10 ms out, 10 ms in) instead of a flush's 5 ms ramp. Clean under ThreadSanitizer
and AddressSanitizer with UBSan.

**Through PipeWire**, driven from the terminal: seek to 100 s, seek by -30 s (heard at
71.5 s), previous at 72.9 s (back to 0.0), next, previous within three seconds (the row
before), jump to row 2: each as asked, 0 underruns.

**Not done here, and noted:**

- Control-to-ear through PipeWire for a seek and a skip is for the close.
- A source that fails its seek (a damaged file) fails, rather than playing from where
  the decoder could get to.

## Milestone 4, stages and the aligned recut

Nov 23, 2026. Linux laptop, `linux-dev`.

**Two decisions first**, made at the start of the milestone: the path is always ready
(the source is always kept, and no recut ever seeks or decodes), and the 20 ms target
counts from the change reaching `hifi` to the join's first frame leaving the callback,
leaving the engine's tick and the device out as platform latency.

**As built:**

- The stage contract is `korvid/hifi/stage.h`: `open`, `set`, `process` (f64, in place,
  full scale +-1), `flat`, `run_in`, `reset`, over memory handed in. The gain stage is
  `hifi_eq`'s first code; the chain (gain, and the equalizer next) is the player's,
  handed to `hifi_create`.
- The source ring: every decoded frame, in the decoder's integers, keyed by programme
  frame (a count of frames rendered that never resets), with a list of where each
  source began in it. It holds the lead and half a second behind the frame heard. Every
  slot's ring, the first render's too, is `render_frames` over it: the integer path when
  every stage is flat (the slice 1 conversion, unchanged), otherwise f64 through the
  chain and back with TPDF dither from a hash of the source frame and the channel. A
  source's first frame resets the chain.
- The recut: when the heard chain's parameter generation is not the one wanted, the
  render thread opens the other slot's chain fresh, takes the next frame the callback
  will take as the start, runs in (the longest stage's run-in, not past its source's
  start), renders the first two chunks and posts the request, then renders the rest of
  the lead behind it a chunk at a time. From then on new frames are decoded into it.
  The callback joins when the new ring holds the frame it is at and a fade more:
  linear, 10 ms, frame for frame in double, rounded, no dither. Paused, it switches
  with no fade at play. A jump that comes while a recut's fade is under way waits for
  it, at most a fade.
- One recut in flight: changes that arrive meanwhile update what is wanted, and the
  next recut renders the newest. `ParamsHeard` names the generation heard; the player
  says `PlayerParamChanged` once per parameter at the value heard.
- `hifi_status` gained `flat`, `params_heard`, `recuts`, `recut_seconds` (the last
  recut's change-to-join time); `exact` now also needs a flat chain and no join. The
  player gained `PlayerSetParam`, `PlayerParamChanged`, `player.chain`, `joining`,
  `recut_seconds` and `recuts` in `player.output`, and `player volume`, `mute`, `set`.

**Found:**

- The step criterion ("no step larger than the larger of the two renders' own steps
  there") cannot hold as worded at a change of level. A linear crossfade's step is
  (1-t) of the old one's, t of the new one's, plus the difference between them spread
  over the fade; at a waveform's peak both renders' steps are near zero and the third
  term is not. The test bounds the step by the larger of the two and that slope (plus
  a bit for rounding), which is what "no click" means for a fade between levels.
- Dither on an exact zero turned mute into noise of one bit. An exact zero now stays
  zero.
- The first recut through PipeWire took up to 19.5 ms: the whole five-second lead was
  rendered before the join was asked for. Asking after the first chunk and rendering
  the rest behind it brought every change under 13 ms.
- Under ThreadSanitizer the render thread is slow enough that tests which drained the
  lead after 20 ms without growth took it for the end of the stream and underran. The
  player tests' pacing now waits a fifth of a second, and slice 1's `play_until` paces
  the same way.

**Tests.** `hifi`, with a gain stage of the tests' own: thirteen recuts with nothing
changed, flat, give the file's MD5; a dozen at -6 dB give a straight -6 dB render bit
for bit; a change from -6 to -12 dB is the first render to the join, the fade's mix of
the two to the sample, then the second, with no step past the bound above; 200 changes
dragged over four seconds of audio take about twenty recuts and end exactly at the last
value, with no underrun; a change while paused is heard from the resume with no fade.
`hifi_eq`: the gain stage alone. The player: volume and mute through events, the row,
`exact`. Clean under ThreadSanitizer and AddressSanitizer with UBSan.

**Through PipeWire** (44.1 kHz, PipeWire's 512-frame quantum at 48 kHz): 20 volume
changes from the terminal, each heard 2.2 to 12.9 ms after it reached the path, 8.6 ms
on average, 0 underruns. What is left in that is the render thread's nap when the lead
is full (5 ms at most, for want of a timed wait: KR10) and the wait for the callback
(a period, about 11 ms here). The tick before it and the device after it are not in it.

**Not done here, and noted:**

- The equalizer, and with it an IIR recut and a non-zero run-in (milestone 5).
- The tick's and the device's share of control to ear, for the close.

## Milestone 5, the equalizer

Nov 23, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- Ten RBJ-cookbook biquads in f64, transposed direct form II: peaks, a low shelf at
  31.25 Hz and a high shelf at 16 kHz, octaves between, all at 0 dB. Forty parameters,
  four a band (kind, frequency, gain, Q). Flat is every gain at 0.
- The attenuation ahead of the bands is the peak of the combined magnitude response,
  found on a 1024-point log grid from 10 Hz to Nyquist and refined by golden-section
  search. It and the coefficients are worked out on the first `process` after a change:
  a recut opens a chain and sets forty values, and that is one design, not forty.
- The run-in is twelve time constants (Q / (pi f)) of the lowest band that is not flat,
  at most half a second, which is what the source ring keeps behind the frame heard.
- Seven presets as data, one of them (`extreme`, every band +24 dB, some narrow) only to
  prove the attenuation. `player eq preset NAME` sends forty values in one tick: one
  recut.

**Numbers.** The measured response within 0.03 dB of the design at worst, over six band
settings alone and with a second band, at ten frequencies each (the criterion is 0.1).
No preset raises a full-scale sine past full scale at any of sixty frequencies from
20 Hz to 20 kHz: `extreme` comes out at exactly 1.00000. With the rock preset left
unchanged, six recuts in the path leave 23 of 48 000 samples a bit off a straight render
and none further: the run-in settles the filters to well under the dither, but they do
not forget, so a recut with an equalizer is close rather than bit for bit. Through
PipeWire, presets switched from the terminal were heard 2.7 to 11.5 ms after reaching
the path, 0 underruns.

**Not done here, and noted:**

- The attenuation is for sines; a full-scale square through a boost can still go over,
  and is clipped at the device's word.
- Matched designs near Nyquist and coefficients from the engine's transcendentals, as
  the PRD defers.

## Milestone 6, the queue

Nov 23, 2026. Linux laptop, `linux-dev`.

**As planned**, with these particulars:

- The player's items live in fixed slots and the list is an order of them, so a read or
  a piece given back keeps pointing at its item through any edit. Ids are handed out in
  order and stay through moves; rows carry `pos`. "Done" is no longer a state: a row is
  done by being before the place (`player.transport`'s `place` and `place_pos`), and its
  state then says idle (or failed), with how it ended in `damaged` and `reason`.
- `PlayerQueueEdit` (remove, move, clear) and `PlayerEnqueue` with `before`. A removed
  row leaves the table at once; its slot waits for `hifi`'s `Released`.
- After edits the player works out what should follow the place's row and, if `hifi` has
  something else, calls `hifi_replace_after` (new): while that row is still being
  decoded it is free; once its end is in the lead, `hifi` cuts the lead there with an
  aligned recut (the same audio to the boundary from the source ring, then the new
  programme) and a cut note that voids the old programme's marks past the boundary.
- CLI: `player remove`, `move`, `insert`, `clear` (the rest of BR26).

**Found on the way, a bug from milestone 4:** a jump's marks, made before the callback
had joined it, carry the slot's frame and are placed when "the last request joined" is
the jump. A recut joined afterwards changed that, so a mark made late (a second source's
`Started`) could never be placed, and as the head of the caller's list it held back every
mark behind it. Through PipeWire: seek near the end of a track, remove the next, and one
time in five the track after never said it had started, though it played. The callback
now keeps the last jump it joined and where, apart from recuts, and a render checks
whether it has been joined before it makes a mark relative. Twenty runs of the same
sequence after: all right. A test makes recut-after-jump deterministic, and fails on the
old code.

**Tests.** `hifi`: replacing after the source playing before its end is rendered (no
recut) and after (a recut), and with nothing: exact both sides, the old next never
Started; marks across a jump and a recut. The player: remove and move inside the lead
and before it, each heard as the edit with the recut where expected; insert before a row;
remove the row playing; clear. ThreadSanitizer and AddressSanitizer clean.

**Through PipeWire and the capture device**, on the real album: seek to 251 s of the
first track, remove the second (already in the lead): a recut 6 to 8 ms after the edit
reached the path, and the third track follows the first.
