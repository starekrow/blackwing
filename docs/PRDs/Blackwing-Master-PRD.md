# Blackwing — Master PRD

Nov 22, 2026 · draft, first review folded in · corrected Nov 23 where slice 1 proved it wrong

## What it is

Blackwing is a hi-fi local music player built on the Korvid engine: Korvid's Krome for the
UI, Korvid's I/O, threads, events and tables for everything else. You aim it at a
directory and start playing. Under the friendly player is an audio path that can say, and
later prove, what it did to the samples.

The design discussion is
[docs/design/design-notes-2026-11-22.md](../design/design-notes-2026-11-22.md). It
describes several products at once: a player, a library, a ripper, a plugin host, a
verification lab and a phone app. This document turns it into something to build by
putting it in order. Three phases:

- **P0, the MVP.** A player that is good to use every day: plays the common formats
  without glitching, finds your music by itself, looks like whatever you want, answers
  the media keys. No ripping, no modules loaded at runtime.
- **P1, the cool stuff.** Gapless nailed, sandboxed native modules, tag write-back,
  digital loopback, more visualizers, deeper system and hardware integration. P1 should
  start to feel like a specialist's tool that just happens to have a really friendly
  player over the top.
- **P2, the infill.** The rest of the library formats, the optional DSPs, the headphone
  profile picker, the cooker.

Slice 1 has built the audio path's spine: `hifi_codec` with FLAC, `hifi_platform` with
PipeWire and a capture device, `hifi`, `blackwing_player`, and a headless `blackwing`
that plays files named on its command line. The other modules are scaffolds:
[the module directories](../../modules/README.md), each with a `module.lua` and a page
saying what it will hold.

## Goals

- **Audio never glitches.** It is the first priority, above the display and above
  everything else, and the design is arranged around it.
- **The display answers at once.** Low latency and low worst-case frame times; free when
  still; asleep when hidden.
- **Automatic.** One directory, no import step, no dialog about tags. Music is playable
  before the scan has finished, and what you look at is scanned next.
- **The player is an API.** The core is driven by events and read through tables on
  Korvid's busses. A UI is one customer of it, and there are four.
- **Never write to the user's music unasked.** P0 does not write to an audio file at all.
- **Failure is graceful.** A bad file, a vanished share, a pulled DAC: each costs one
  message and at most one track, never the session.
- **Claims that can be checked.** What the pipeline did to the audio is computed from
  what the stages declare (P0), then measured (P1).
- **A demonstration of Korvid.** Long uptime, an idle window, text-heavy lists and, in
  P1, crash-and-respawn in the middle of a song.

## Non-goals

- **Mobile.** No iOS or Android app, no sync. The core must not preclude one (see
  [Not precluding mobile](#not-precluding-mobile)).
- **Ripping and burning.** [Future work](#future-work). Reading music out of a disc image
  someone already has is in (P1).
- **Runtime module loading in P0.** Everything in P0 is linked in. Skins and screens are
  data, not modules, and load at runtime in every phase.
- **Streaming services and DRM.** Local files, and later radio streams.
- **Video.**
- **Writing a text or accessibility stack.** Blackwing uses what Krome has and asks Krome
  for what it lacks (see Open).

## Priorities

Decided in the design discussion, and the tie-break for everything below:

1. Audio (never glitches).
2. Display responsiveness (except when hibernating off-screen).
3. Everything else.

## Frame

- **Modules.** All of them start in this repository, under `modules/`. Engine modules they
  build on (`core`, `platform`, `krome`, `lua`) stay in Korvid.

  | module | holds | phase |
  |---|---|---|
  | `hifi` | the hard-realtime audio path and its interfaces: sources, stages, the lead, the join, the output callback, the clock at the DAC | P0 |
  | `hifi_codec` | the decoder contract and the decoders: a file handle in, PCM and gapless facts out | P0 |
  | `hifi_eq` | the equalizer, a stage on `hifi`'s stage interface | P0 |
  | `hifi_platform` | the output device per OS: enumerate, open at a format, exclusive where it exists, hot-plug, reported latency, the realtime thread | P0 |
  | `media_platform` | the OS's media surface per OS: media keys in, now-playing out, keep-awake, interruptions | P0 |
  | `blackwing_player` | the player with no face and no library: the queue, the transport, and the events and tables that are its whole API | P0 |
  | `blackwing_tags` | reading tags from every source, ranking them, sidecar overrides; no library, no database | P0 |
  | `blackwing_library` | roots, the scanner and its priority queue, the sqlite index, change watching, queries | P0 |
  | `ui_dev` | the development face: every table and event visible, the pipeline inspectable | P0 |
  | `ui_wsz` | the classic face: Winamp `.wsz` skins | P0 |
  | `ui_clean` | the clean face, for the not-Winamp crowd | P0 |
  | `ui_compact` | the clean face, compact | P0 |
  | `blackwing` | the application: the executable, the module list, the command line, the settings, which face is up | P0 |

- **Three layers.** `hifi*` is the audio path: hard-realtime data flow and the interfaces
  around it. It knows nothing of tracks, queues or tags, and renders what it is handed.
  `blackwing_player` is a player that another project could link: it turns "play this,
  then that" into sources, stages and recuts on `hifi`, and has no face and no library.
  `blackwing`, with the library, the tags and the faces, is the application around it.
- **Names.** `platform` is a suffix here (`hifi_platform`, `media_platform`), so a module
  and its platform half sit together in a sorted listing. Korvid's own modules put it
  first (`platform_audio`, `platform_hid`). The notes' `hifi_core` is `hifi`.
- **`hifi_platform` is new, not `platform_audio` extended.** Korvid's `platform_audio`
  opens the default device through SDL3 on the desktop, one stream, with no device choice,
  no exclusive mode and no hot-switching. Blackwing does not link it. Whether the two
  converge later is the engine's question.
- **One direction.** The faces depend on `blackwing_player` and `blackwing_library` for
  their declarations only. The player depends on no face and no library: it plays what
  it is handed. `hifi` depends on no player.

  ```
  ui_dev  ui_wsz  ui_clean  ui_compact            media_platform
      \      |       |        /                         |
       events and tables on Korvid's busses ------------+
          /                        \
  blackwing_player            blackwing_library
          |                          |
        hifi --- hifi_eq        blackwing_tags
        /   \
  hifi_codec  hifi_platform
  ```

- **Third-party code in P0**, all permissive: dr_flac, dr_mp3 and sqlite. Licences are checked when each is vendored, and each
  ships its licence file through its module's `licenses`.

## The pieces

### The player as an API

`blackwing_player` has no functions a face calls. It has **commands** (events it consumes),
**state** (tables and resources it writes and anyone reads) and **notifications** (events
it emits). Everything a face can do, a Lua script, the command line, `media_platform` and
a test can do the same way, and a face that crashes or is swapped takes nothing of the
player with it.

A sketch, to show the shape. The first slice's PRD settles the names and fields.

| name | kind | what |
|---|---|---|
| `player.transport` | resource | stopped, playing, paused or starting; the track; the position at the DAC; the lead |
| `player.queue` | ktable | a row per queued item: what it is, where it comes from, ready, failed and why |
| `player.output` | resource | the device, its format and mode, its latency, the bit-perfect state |
| `hifi.devices` | ktable | the output devices there are, and what each can do |
| `hifi.chain` | ktable | a row per pipeline stage: its parameters, whether it passes audio unchanged now |
| `hifi.meter` | resource | levels and spectrum, timed to the DAC, for the visualizer |
| `PlayerPlay`, `PlayerPause`, `PlayerStop`, `PlayerSeek`, `PlayerNext`, `PlayerPrevious` | events in | the transport |
| `PlayerEnqueue`, `PlayerQueueEdit` | events in | the queue |
| `PlayerSetParam`, `PlayerSelectDevice` | events in | a stage's parameter (volume is one); the output |
| `PlayerTrackStarted`, `PlayerTrackEnded` | events out | emitted in the tick after the DAC reached them |
| `PlayerTrackFailed`, `HifiDeviceChanged`, `HifiUnderrun` | events out | what went wrong, with the reason in words |
| `PlayerQueueEnded` | event out | everything queued is done or failed, and nothing sounds |

Every notification carries `seq`, a count across all of them: Korvid keeps the order of
events within a type, not across types, so a `TrackEnded` and the next `TrackStarted`
from one tick reach their handlers in the schedule's order. What is playing is read from
`player.transport`, not kept from events.

The `player.*` names are the player's. The `hifi.*` ones are the audio path's, and the
player publishes them: `hifi` is a library, not an engine module, so it has no busses of
its own (slice 1 found this, and published the output as `player.output`). Between the
player and `hifi` the interface is `hifi`'s own: ordinary calls that queue a message and
return, and a poll. Slice 1 settled that this is not events: `hifi` has one caller, below
the level a face or a script reaches.

The library is the same shape: `LibraryAddRoot`, `LibraryQuery` (with an id, so a newer
one cancels it), `LibraryPrioritize` in; `library.roots`, `library.scan`, result pages
and `LibraryChanged` out.

Two rules make this hold. **Time is the DAC's**: every position and every "started" is
when the listener hears it, from the device's reported latency, never where the decoder
is. **No pointers cross**: rows and events are plain data and handles, so the same API
works across a process boundary in P1 without a second design.

### Loading through codecs

A decoder is given the file as a sparse span (its address range, and a map of which
pieces of it have arrived) and memory to work in, and returns a format, a length,
gapless facts (delay and padding where the container has them) and blocks of PCM, as
integers at the file's depth, from the start or from any frame it is sent to. It does
not open files, name paths, allocate or call back into the player, and it waits by
returning: "I need these bytes". The player reads the file in pieces, the piece a decoder
waits for first, within a cap on bytes held, so a track starts on its first piece. In
P0 decoders are linked in; the contract is written so that one can move into a
sandboxed process in P1 unchanged. (Slice 1 read the whole file first; slice 2 made it
pieces.)

P0 decodes FLAC and MP3. P1 gains the usual suspects (WAV, AIFF, Ogg Vorbis, Opus, ALAC,
AAC and their kin) and the ability to run someone else's native codec, as a sandboxed
module on the same contract. The cost in a decoder is malformed files, so the broken-file
corpus starts with the first one.

### The lead and the recut

The mechanism is `hifi`'s; deciding when a change calls for it is the player's.

**Playback gets out in front.** The pipeline renders finished audio (decoded, processed,
at the output's format) some seconds ahead of what is being heard. That distance is the
**lead**: how much finished audio is buffered ahead of the playhead. (Not "the buffer":
there are several buffers on this path, and the lead is a length of time, not one of
them.) The output thread only takes from it; a stall on a disk or a share has seconds
to resolve before anyone hears it. The next track's bytes are read while the current one
plays, within a memory cap.

**A change restarts the mix closer to now.** A lead of seconds would make every control
feel seconds late. So when something changes what should be heard (a seek, a skip, an EQ
band, the volume, the next track being replaced), the pipeline does a **recut**: it
starts a second render at a point just ahead of the playhead, with a short lead, and the
output moves from the old render to the new one. The old render stays valid and keeps
playing until the new one is ready, so a slow start is heard as a slightly later change,
not as silence.

**A crossfade covers the uncertainty.** How long a render takes to start is not known in
advance: the file may be cold, the codec may have to seek, a filter needs a run-in to
settle. So the join is not a sample-exact splice at a predicted instant. It is a short
crossfade that begins when the new render has enough audio, wherever the playhead is by
then. For a parameter change the two renders are the same music at the same position, and
the crossfade also hides the step in the parameter. For a seek or a skip the old audio
fades out at once and the new fades in when it arrives.

What this costs: the stretch being crossfaded is modified audio, and the indicator says
so for as long as it lasts.

**As built in slice 2.** The join runs in the output callback, as two gains over two
rings. The pipeline keeps the decoder's own samples from half a second behind the
playhead to the front of the lead, always, so a parameter change never seeks or decodes:
it runs the chain again over samples in memory and is heard within a callback or two
(2 to 13 ms through PipeWire, counted from the change reaching the path; the tick
before and the device after are the platform's). A seek or a skip fades out over 10 ms
and fades in over 10 ms once the new render has 50 ms. The lead is five seconds, the
aligned crossfade 10 ms and linear. One thing slice 2 found: "no step at the join
larger than either render's own" cannot hold at a change of level, since a fade's step
at a waveform's peak is at least the level change spread over the fade; that slope is
part of the bound.

### The output

`hifi_platform` opens a device at a format and pulls from a callback on a realtime thread
of the OS's audio class: the backend's own where it has one (PipeWire's data thread on
Linux, which PipeWire puts in the class itself), or one `hifi_platform` makes and asks for
the class (Korvid's `os_thread_enter_audio_class`). P0 asks each device for the track's
sample rate and gets out of the way when it can (the device switches rate, exclusive
where the OS has it); when it cannot, `hifi` resamples and dithers and says so.

A shared device can take any rate and resample it itself, out of sight. PipeWire does
this: it accepts the track's rate and converts to its graph's, which on a stock desktop
allows 48 kHz only. Such a device reports itself as shared, and the indicator reads
*modified*, whatever `hifi` did. A device that goes away pauses playback
and names itself; one that comes back, or a default that changes, is an event, not a
restart of the program.

The **bit-perfect indicator** is computed in P0 from what each stage in `hifi.chain`
declares: *identical* when every stage passes audio unchanged and the device took the
format as it is, *modified* otherwise. The claim ends at the OS API; what a driver does
after it cannot be seen from here until the loopback in P1.

### Gapless

P0 provides for it: consecutive tracks are joined in one output stream without reopening
the device, and the decoder contract carries delay and padding. Lossless files of one
format join sample-exactly. P0 does not promise every MP3 and every container, and a
change between tracks that the open device cannot carry (the rate, the channel count, or
a depth wider than its word) reopens it with a gap. P1 nails it everywhere and
makes rate-change behaviour a setting.

### Tags

**Read-only from sources, overridden in sidecars.** Nothing is "the right tag". Each
source (embedded tags of each kind, a cue sheet, the file's name and path, a folder's
image) has a specificity rank, and the best-ranked value for each field wins. The user's
own corrections are overrides in a sidecar, which outrank every source and which no scan
touches. P0 never writes into an audio file.

A file whose tags cannot be read is still a track: it has a name from its path and it
plays.

### The library

**Aim it at a directory, and you're done.** There is no import step and nothing to
confirm. Scanning is passes, each useful on its own:

1. **Names.** Walk the tree. Every audio file is listed and playable from this pass
   alone, shown by folder with tags guessed from its path.
2. **Tags.** Read each file's tags and art, fill the index, and let the browsing views
   (artist, album, genre) fill in as it goes.
3. **Background.** Whatever is slow and nobody is waiting for: thumbnails now, audio
   hashes and loudness later.

**Looking at something scans it next.** Opening a folder, an album or a search result
that has not had its tags read moves it to the next-to-scan slot, ahead of the walk.
Playing a file reads its tags at once.

The index is sqlite, kept locally, a cache that can be deleted and rebuilt from the
directories and the sidecars. Changes come from the cheapest watching the OS has, with
polling for network shares. Queries are asynchronous and cancellable, and lists are
virtualized: fifty thousand tracks is the size to design for.

### Disc images

Some people's music is a disc image and nothing else. In P1 the library looks inside
one as it looks inside a directory, read-only: the image is a root or sits in one, and
its tracks are rows like any other. A decoder is already handed a handle rather than a
path, so a track that is a range of bytes inside an image needs nothing new from the
codecs. An `.iso` is a data filesystem (ISO 9660 or UDF) holding ordinary music files,
and that is the P1 case. An image of an audio CD (`.bin` with `.cue`, and the like) is a
different format: raw CD audio with a table of tracks, so its tracks are ranges of PCM
with the cue sheet as their tags. That is P2. SACD images are open.

### The faces

The UI is not part of the player. Each face is a module that draws with Krome, reads the
player's tables and emits its commands. Four, because they have different jobs:

- **`ui_dev`**, built first. It is how Blackwing is developed: every table, every event,
  the pipeline's stages, the lead, the scan queue. Plain, dense, complete.
- **`ui_wsz`**, classic Winamp skins. A `.wsz` is a zip of bitmaps for a fixed layout, so
  this face is an interpreter for that format, and the proof that a face needs nothing
  from the player but the API.
- **`ui_clean`**, a modern library-first player for people who do not want 1998.
- **`ui_compact`**, the clean face in a small window: transport, what is playing, a queue
  that folds away.

`ui_clean` and `ui_compact` are separate modules that should share their screens' parts.
All four idle for free: nothing drawn when nothing changes, nothing running when hidden.

### The system

`media_platform` is a small subscribe-and-send layer over each OS's media surface: MPRIS
on Linux, SMTC on Windows, the Now Playing and remote command centres on macOS. Commands
come in as the same events a face emits; what is playing, its art and its position go
out. It also holds the system awake while playing and reports interruptions. The player
does not know it exists.

### Failure

Stated once, because it applies everywhere. A failure is local, named and survivable:

- A file that will not decode fails its own queue row with a reason, and the next track
  plays. A file that fails part-way plays what it had.
- A scan error marks the file and moves on. The scan never stops for one file or one
  directory.
- A root that disappears (an unplugged disk, a share) keeps its index and shows as
  offline; nothing is forgotten, and it resumes when it returns.
- A device that disappears pauses. No output device at all still lets you browse.
- A corrupt index is rebuilt. A corrupt sidecar is set aside, not overwritten.
- A skin that will not load falls back to the last good face.

### Not precluding mobile

Mobile is out of scope, and these keep it possible:

- `hifi`, `hifi_codec`, `hifi_eq`, `blackwing_player`, `blackwing_tags` and `blackwing_library` use no
  desktop API directly; what
  is per-OS is behind `hifi_platform` and `media_platform`.
- Files reach decoders and tag readers as handles the player was given, not as paths
  they open, so a sandboxed container or a picked document works.
- Nothing in P0's design needs a second process. P1's native modules are an addition on
  the desktop; script modules are the phone's form of them.
- The faces are separate modules, so a touch face is a fifth one.
- The pipeline's memory (the lead, the preload) is capped by a number, not by "a track".
- An interruption (a call, another app taking the audio) is already an event.

## Requirements

| | requirement | phase |
|---|---|---|
| B1 | `blackwing_player`'s whole API is events and tables on Korvid's busses; no face calls a function of it | P0 |
| B2 | Every command a face can give, a Lua script, the command line and a test can give | P0 |
| B3 | Positions and "started" events are in DAC time, from the device's reported latency | P0 |
| B4 | The output callback takes from ring buffers and does at most the join; no I/O, allocation, locks, logging or script on that thread | P0 |
| B5 | The pipeline holds a lead of seconds; the next track's bytes are read ahead within a memory cap | P0 |
| B6 | A control that changes what is heard recuts the mix near the playhead and crossfades into it; the old render plays until the new one is ready | P0 |
| B7 | Seek and skip start from a small chunk at the target, then refill | P0 |
| B8 | Decoders take bytes and memory, return PCM and gapless facts, open nothing, allocate nothing and call back nothing | P0 |
| B9 | FLAC and MP3 decode, including the files in the broken-file corpus that can be decoded | P0 |
| B10 | A native output backend on Linux, Windows and macOS, on the OS's realtime audio thread class | P0 |
| B11 | The device is asked for the track's rate; otherwise the pipeline resamples and dithers (TPDF) | P0 |
| B12 | A device unplugged, plugged or changed as the default is survived and reported | P0 |
| B13 | The bit-perfect indicator, two states, computed from what the stages declare and whether the device is shared | P0 |
| B14 | Volume and a parametric EQ with presets, pre-attenuated so a preset cannot clip | P0 |
| B15 | Consecutive tracks join in one stream; lossless at one rate joins sample-exactly | P0 |
| B16 | Adding a directory is the whole of setting up a library | P0 |
| B17 | Files are playable after the names pass, before any tag is read | P0 |
| B18 | Looking at something unscanned makes it next to scan | P0 |
| B19 | Tags come from ranked sources, read-only; overrides live in sidecars; no audio file is written | P0 |
| B20 | The index is local sqlite, crash-consistent, and can be rebuilt from the roots and the sidecars | P0 |
| B21 | Changes on disk are noticed without a rescan; network shares are polled | P0 |
| B22 | Queries are asynchronous and cancellable; lists of fifty thousand rows scroll without a hitch | P0 |
| B23 | Every failure in [Failure](#failure) behaves as written there | P0 |
| B24 | Four faces: `ui_dev`, `ui_wsz`, `ui_clean`, `ui_compact`; one is up at a time | P0 |
| B25 | A still face draws nothing; a hidden face runs nothing | P0 |
| B26 | Media keys, now-playing and keep-awake on Linux, Windows and macOS | P0 |
| B27 | One visualizer (spectrum and scope), timed to the DAC | P0 |
| B28 | The queue and the session survive a restart | P0 |
| B29 | Nothing in the core precludes mobile, as listed above | P0 |
| B30 | Gapless everywhere: every codec's delay and padding, and a setting for a rate change between tracks | P1 |
| B30a | The usual codecs: WAV, AIFF, Ogg Vorbis, Opus, ALAC, AAC and their kin | P1 |
| B31 | Native modules (decoders, DSPs) loaded at runtime, each in a sandboxed process, audio over shared rings, output treated as hostile | P1 |
| B31a | Someone else's native codec runs as such a module, on the decoder contract, with no change to the player | P1 |
| B32 | A module that hangs or crashes is bypassed or respawned mid-song, and named | P1 |
| B33 | Script modules loaded at runtime | P1 |
| B34 | Exact mode: pinned floating-point behaviour and seeded dither, so a pipeline's output is reproducible bit for bit | P1 |
| B35 | The indicator's third state, measured rather than declared | P1 |
| B36 | A/B of two stacks on aligned input: instant switch, level-matched ABX, live null | P1 |
| B37 | Digital loopback: play into an input and verify what came back | P1 |
| B38 | Tags written back into files when the user prefers it, safely | P1 |
| B39 | More visualizers | P1 |
| B40 | Import and export of other library and playlist formats: the mechanism and the first few | P1 |
| B40a | Music inside a disc image (`.iso`) is browsed and played in place, read-only | P1 |
| B41 | System integration: file associations, single instance, drag and drop, enqueue from the command line | P1 |
| B42 | Hardware integration: exclusive mode on every OS that has it, hardware volume, per-device settings, DSD over PCM | P1 |
| B43 | File identity by a hash of the audio alone, so tracks survive moves and retagging | P1 |
| B44 | Loudness scanning (EBU R128) and normalisation by track or album | P1 |
| B45 | The rest of the library and playlist formats | P2 |
| B45a | Audio CD images (`.bin` with `.cue`, and the like) are browsed and played in place, read-only | P2 |
| B46 | The optional DSPs: linear-phase EQ, crossfeed, convolution and the like | P2 |
| B47 | The headphone profile picker (AutoEQ's profiles) | P2 |
| B48 | The cooker: "record this album with this stack" writes a new directory with the EQ, DSP, tag and timing changes baked in | P2 |


## Phases

### P0: the MVP

Slices, as a proposal; each gets its own PRD when it starts.

1. **Sound out** ([Blackwing-Slice-1-PRD.md](Blackwing-Slice-1-PRD.md)). `hifi_platform` on Linux, `hifi_codec` with FLAC, `hifi` with
   the lead, `blackwing_player` with the transport's events and tables, and the executable playing files named
   on the command line.
   *Done when* a FLAC file's samples, captured at the device boundary, are the decoder's
   samples exactly, and ninety seconds play with no underrun while the disk is busy, from
   the built-in disk and two USB drives. An hour-long run is owed after the gate.
   *Built* Nov 23, 2026, through milestone 5; the slice's PRD has what is still owed.
2. **The recut** ([Blackwing-Slice-2-PRD.md](Blackwing-Slice-2-PRD.md)). Seek, skip, the queue, volume and `hifi_eq`, all through the recut and
   the join; tracks joined in one stream.
   *Done when* a control is heard within the target on a warm file, a cold file is heard
   late rather than as silence, and no join clicks.
   *Built* Nov 23, 2026, through milestone 7; the slice's PRD has what is still owed.
3. **The library** ([Blackwing-Slice-3-PRD.md](Blackwing-Slice-3-PRD.md)). `blackwing_tags` and `blackwing_library`: roots, the three passes, the priority
   slot, sidecars, watching, queries; MP3; the broken-file corpus.
   *Done when* a fresh install aimed at fifty thousand tracks is playing a chosen one
   within seconds, and nothing in the corpus stops the scan.
4. **The development face.** `ui_dev` over the API and nothing else.
   *Done when* everything in slices 1 to 3 can be driven and watched from it, and from a
   Lua script with no face at all.
5. **The system.** `media_platform`; the Windows and macOS backends of both platform
   modules; hot-plug.
   *Done when* the media keys and the OS's now-playing surface work on all three, and
   pulling the DAC pauses.
6. **The faces.** `ui_wsz`, `ui_clean`, `ui_compact`, the visualizer.
   *Done when* a stock `.wsz` skin drives the player, and each face idles at nothing.

**Done, for P0:** someone who has never seen it installs it, points it at their music
and uses it as their player for a week, on any of the three desktops, without reading
anything and without a glitch.

### P1: the cool stuff

B30 to B44, with B30a, B31a and B40a. The order inside P1 is open, with one dependency worth stating: exact mode
(B34) comes before the null and the loopback (B35 to B37), which are only arguments once
output is reproducible, and the sandbox (B31) comes before modules are worth A/B-ing.

**Done, for P1:** a DSP author edits, rebuilds and hears their module swap in without the
music stopping, kills it and loses nothing, and reads the null against the last build.
Everyone else notices only that albums no longer have gaps.

### P2: the infill

B45 to B48, with B45a.

## Future work

Wanted, and in no phase. Each needs its own plan before it gets one.

- Ripping and burning discs.
- Mobile: the iOS and Android apps, and sync.
- Video.
- MilkDrop preset compatibility.
- Internet radio.
- Scrobbling.
- Lyrics and online art and metadata.
- Distribution: signing, notarization, packaging.

## Decided

Nov 22, 2026, in setting up the project.

- **Three phases**, as above. P0 has no ripping and no runtime module loads.
- **The player's API is data and events** over Korvid's busses.
- **The UI is separate modules**, four of them.
- **Tags are read-only in P0**, with overrides in sidecars. Write-back is P1 and by
  preference.
- **Playback leads; a change recuts near the playhead; a crossfade covers the join.**
- **Gapless is provided for in P0 and nailed in P1.**
- **The library is a directory and nothing else**, and attention drives the scan order.
- **Failure is graceful.**
- **Mobile is out of scope** and not precluded.
- **The `hifi` and platform modules start in this repository.**
- **Priorities**: audio, then display, then everything else.

Nov 22, 2026, in review of the first draft.

- **`hifi` is the hard-realtime data flow and its interfaces**, and nothing else.
- **Three layers**: `hifi*`, then `blackwing_player` (linkable into another project),
  then the application. Tags and library are `blackwing_tags` and `blackwing_library`.
- **`platform` is a suffix**: `hifi_platform`, `media_platform`.
- **"Lead" and "recut"** are the words.
- **The draft's placements stand.** In P0: a basic EQ, one visualizer, session restore,
  all three desktops with Linux first. In P1: exact mode, A/B and the null, script
  modules, audio-hash identity, loudness. `media_platform` holds keep-awake and
  interruptions as well as the keys.
- **FLAC and MP3 in P0.** The usual suspects, and someone else's native codec, in P1.
- **Disc images are read, not ripped**: music inside an `.iso` plays in place (P1; the
  phase is this draft's guess). Audio CD images follow in P2.
- **Everything unphased is future work.**

Nov 23, 2026, by slice 1.

- **`hifi` is called, not sent events**, and the player publishes what it reports.
- **Notifications carry `seq`**; order across event types is not something to rely on.
- **The lead is in the device's own format**, so with no stage the path is integers end
  to end. Where the change to floats sits is slice 2's question.

## Open

- **The library as a module or as part of `blackwing`.** This draft keeps
  `blackwing_library` a module, because the faces need its declarations and a library
  module cannot depend on the executable. Folding it in means folding the faces'
  library views in too.
- **`hifi_eq` as its own module**, added by this draft: once `hifi` is only the flow and
  its interfaces, the EQ is a stage beside it. Volume sits there for now.
- **The licence**: Apache 2.0, as Korvid is, or MIT.
- **Where sidecars live**: beside the music (portable, visible to other machines) or in
  Blackwing's own data (works on read-only media), or beside with a fallback. And their
  format and what they key on before B43.
- ~~The recut's numbers and where the join runs~~: settled by slice 2 (see The lead and
  the recut).
- **AAC on Linux.** libopus and Apple's ALAC are permissively licensed. AAC has no
  comparable decoder; the OS's own cover Windows and macOS but not Linux.
- **SACD images.** Data `.iso` (P1) and audio-CD images (P2) are settled. An SACD image
  means decoding DSD, and is in no phase.
- **The resampler.** The notes say "soxr-class"; libsoxr itself is LGPL. Write one or
  find a permissive one.
- **How a face is chosen and whether it can change while playing.** Nothing in the
  design prevents a live switch.
- **How much `ui_clean` and `ui_compact` share**, and whether the classic face is Krome
  screens or its own drawing over Krome's primitives.
- **Text and accessibility.** The notes want complex shaping, collation and an
  accessibility tree. Krome's plan has accessibility late and its own text. What
  Blackwing needs from Krome, and when, is a conversation with the engine.
- **The indicator's middle state.** The notes define it as a deviation under half an LSB
  that "can't change the DAC word". It can, when a rounding boundary lies between the
  two values. B35 needs a definition that holds: at most one LSB, or below the dither.
- **Whether `hifi_platform` and Korvid's `platform_audio` should become one.**
- **Playlists in P0**: reading M3U8 is small, and the queue alone may not be enough.
