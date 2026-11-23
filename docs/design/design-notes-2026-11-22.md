# Korvid Music Player — Design Notes

Nov 22, 2026. Source: design discussion. Captures decisions made and issues raised.
"Decided" = settled direction; everything else is a consideration to resolve during design.

This is a design record: it is kept as it stood on its date. The project took the name
Blackwing afterwards. What is being built, in what order, and where the plan departs from
these notes is in [the master PRD](../PRDs/Blackwing-Master-PRD.md).

## Goal

A cross-platform local music player built on korvid: nice EQ presets, reads libraries
other players are hostile to, rips CDs, open album art/lyrics sources, fully skinnable,
and a bit-perfect, verifiable hi-fi audio stack. Doubles as a demonstration of korvid's
model (isolation, messages, determinism, hot reload). License: Apache 2.0 or MIT.

## Substrate (already exists in korvid)

- krome UI: skinning foundation, sprites/animation, hardware-accelerated rendering
- Async, event-driven I/O; event bus that carries handles; Lua scripting
- File watching (from goldberg); out-of-process monitor protocol (from goldberg)
- Owns threading model, process model, FP behavior
- Stable transcendentals vetted across 5 platforms, plus a slower near-exact family
- iOS support; paid Apple developer account in hand
- Additional render fallbacks (Vulkan 1.1, OpenGL) wanted but not blocking — player just
  runs slower until they land

## Priorities (decided)

1. Audio (never glitches)
2. Display responsiveness: low latency, low worst-case timings (except when hibernating
   off-screen)
3. Everything else

Video playback (music videos etc.) is P5. CD burning is low priority.

## Module list (proposed)

- `hifi_core`: playback pipeline, output (separate from game audio: similar API, more HW
  control, fewer voices)
- codecs
- tags/library (local sqlite for tags and meta; I/O async)
- `cdrom_platform`, `cdrom_burning`
- `hifi_eq`
- `hifi_visualizer` host
- `mediakeys_platform`: minimal subscribe/send abstraction
- Player fully wired into the engine's data model

## Audio output

- Bit-perfect output with sample-rate switching: WASAPI exclusive, CoreAudio hog mode /
  nominal rate, PipeWire
- Good resampling (soxr-class) when switching isn't possible
- Survive devices being plugged/unplugged
- Real-time thread class per OS: MMCSS Pro Audio (Windows), audio workgroups (macOS),
  rtkit (Linux)
- Output callback only copies from ring buffers. No sqlite, I/O, allocation, or scripting
  GC on that path; scripting stays on the control side
- Decoders run seconds ahead; preload the next track fully into RAM (NAS spin-up is the
  real contention)
- Seek/skip: fast-start path (decode a small chunk at the target first, then refill).
  Time-to-first-audible-sample is the latency people feel
- Playhead and visualizer follow the DAC presentation timestamp, not decode position
  (Bluetooth adds 150–250 ms). Feed reported output latency (CoreAudio device+stream
  latency, WASAPI stream latency, AAudio timestamps) across the snapshot boundary
- Internal float32; TPDF dither when reducing bit depth; digital volume breaks bit-perfect
  by definition, so expose hardware volume in exclusive mode
- Keep the system awake during playback; pause when headphones are unplugged; handle
  mobile interruptions (iOS audio session, Android audio focus)

## Gapless

- Metadata is solved: LAME/Xing header, iTunSMPB, Opus pre-skip; FLAC is sample-exact
  natively
- Pipeline: preroll the next track into the same output stream without reopening the
  device
- Conflict: gapless vs. bit-perfect when consecutive tracks differ in sample rate
  (resample vs. switch with a gap). Make it a user setting

## Codecs

- MP3, MOD, Vorbis, FLAC, Opus, AAC/ALAC; SPC (SPC700 + S-DSP emulation)
- Existing permissive decoders (dr_mp3, dr_flac, stb_vorbis, libopus) available; writing
  our own is optional
- Real cost is tolerating malformed files

## Tags / library

- Decided approach: tags are best fit, not "right." Each source gets a specificity rank,
  user can override, sidecars keep info local, global sqlite cache, cheapest change
  monitoring the OS supports
- Write-back: other players and devices read only embedded tags, so editing means writing
  into files. Corruption risk lives here
- Identity / entity resolution is the hard part:
  - Album grouping (same title, different releases, compilations, deluxe editions)
  - File identity across moves and renames: key on a hash of the audio frames only
    (excluding tag blocks) so keys survive retagging
  - MusicBrainz release IDs when present
- sqlite stays local always, in WAL mode, crash-consistent
- Network shares: FS events don't see remote changes reliably, so add a polling fallback
- Library UI: virtualized lists for 50k+ rows. Queries async and cancellable (a keystroke
  cancels the in-flight query). Cover art decoded off-thread into a thumbnail cache. Text
  shaping cached per row, visible rows only

## Ripping

- Secure ripping beyond raw sector decode:
  - Drive read offsets (AccurateRip offset database; redistribution terms are murky)
  - Defeat drive caches so re-reads hit the disc
  - Don't trust C2 error pointers
  - AccurateRip v1/v2 checksums (algorithm publicly documented)
  - Optionally CUETools DB for repair
  - Pregap/HTOA, pre-emphasis, ISRC, CD-Text
- SCSI passthrough per OS: SG_IO, IOCTL_SCSI_PASS_THROUGH_DIRECT, IOKit (plus
  DiskArbitration exclusive access)
- USB bridge chips mangle passthrough; handle via pluggable hardware drivers over time
- Licensing: most ripping prior art (cdparanoia, libcdio-paranoia, whipper) is GPL/LGPL,
  so read for understanding only and write clean-room. EAC is closed

## EQ

- Biquads (RBJ cookbook) as the base
- Fix bell cramping near Nyquist (Vicanek matched designs or oversampling)
- Auto pre-attenuate by max boost so presets don't clip
- Decide on linear-phase support
- Open preset source: AutoEQ headphone-correction profiles
- Coefficient design uses near-exact transcendentals (computed once per preset, cached)
- Parameter smoothing (per-block recompute during drags) uses stable transcendentals.
  Smoothing must land on the cached near-exact coefficients at the end, so typed and
  dragged values give identical filters
- Loudness: EBU R128 ReplayGain scan, track/album modes, true-peak limiting; interacts
  with EQ headroom

## Visualizers

- Eye candy, but sync to DAC presentation time (see Audio output)
- MilkDrop preset compatibility (projectM shows reimplementation is feasible; Slang as
  the shader target) gives a huge free content library

## Skinning / UI

- Decided: both. A default layout that doesn't suck for easy skinning; full layout control
  is "just more JSON"
- Lesson from Winamp 3: flexibility wasn't the failure; making the heavy flexible system
  the mandatory authoring surface was
- Need a versioned widget contract plus fallback docking for widgets a skin doesn't place,
  so new features don't break old skins
- Classic Winamp `.wsz` skin import: documented format, ~100k skins exist (Webamp, Skin
  Museum)
- Hot reload of UI from disk; preserve widget state (scroll, selection, focus, expansion)
  by stable widget IDs so reloads re-bind instead of resetting
- Idle cost: render on demand, damage-only updates, no frame loop when hidden; on iOS stop
  rendering entirely in background. Idle wattage will be compared to foobar's
- Accessibility: publish an a11y tree (NSAccessibility, UIA, AT-SPI). Widget contract
  carries role and label regardless of visuals
- Text: HarfBuzz shaping, font fallback, CJK/RTL/emoji; ICU collation, article-ignoring
  sorts, sort tags (Japanese readings), diacritic-folding search

## Module system (decided direction)

- Runtime-loadable modules from a discoverable location
- iOS/Android: script-only modules. Desktop: script modules work the same; native modules
  run in a separate sandboxed process
- Setup work restricted to part-time on a single core, or yields first under CPU pressure
- Guest process: no rights beyond its own RAM and trading POD events with the player
- Enforce sandboxing per OS: seatbelt / `sandbox_init` (macOS), seccomp-bpf + namespaces /
  Landlock (Linux), AppContainer or restricted token + job object (Windows). Chromium's
  sandbox is BSD-licensed prior art
- Asset access by capability: the player opens the user-chosen file and passes a read-only
  handle (SCM_RIGHTS / DuplicateHandle / Mach port); guest maps and seeks freely and can't
  name other files
- Audio between player and guest via shared-memory ring buffers (latency irrelevant since
  decode runs ahead)
- Treat guest output as hostile: bounds-check lengths and counts; post-guest sanitizer
  (NaN/Inf → 0, clamp, sudden-full-scale guard with ramp to silence)
- DSP modules can't "give way" silently: deadline watchdog, bypass the offender with a
  short crossfade, tell the user which module did it
- Lifecycle, identical for script and native: load → snapshot → replace → discard
  - Unload = kill the process. Generation-stamped handles and channel; stale in-flight
    events are dropped
  - Hang: watchdog + respawn, resume from sample offset (guests stateless or snapshotable)
  - Native hot reload: spawn the new version, hand over the snapshot, swap at a block
    boundary (DSP, short crossfade) or sample offset (decoders), kill the old one
- Granularity: one process per native module by default
- macOS hardened runtime: loading third-party native code in-process would need
  `disable-library-validation`; out-of-process avoids it

## Exact mode / bit-perfect

- Every pipeline runnable in exact mode with fully vetted calculation paths; wide/SIMD
  paths (e.g. future AVX 64-channel low-latency) measured against it, deviation tracked
  and characterized
- Exact mode pins:
  - Fixed reduction order
  - Explicit `-ffp-contract` policy
  - Explicit FTZ/DAZ policy as part of the declared mode (IIR tails hit denormals)
  - No rcp/rsqrt estimate instructions (Intel and AMD differ)
  - Deterministic transcendentals for coefficients
  - Seeded per-stream dither PRNG so dithered output is reproducible
- Deviation unit: relative to output word length (< ½ LSB at 24-bit before dither can't
  change the DAC word)
- Each stage declares whether it passes audio through unchanged at current settings;
  bit-perfect indicator is computed, with three states: identical / below output
  resolution / modified
- Bypassing builtins (raw threads, own I/O) is allowed but visible, Rust `unsafe`-style:
  the module loses exact-mode certification, transparent respawn, and bit-perfect status
- Audiophile scope: serve measurable claims (bit-perfect, resampling, dither, rate
  switching, no dropouts). DSD via DoP is cheap; MQA is dead (administration 2023)

## A/B and module REPL

- Run two module versions concurrently on the same sample-aligned input
- Instant switching; level-matched (~0.1 dB) ABX with hidden X and logged guesses
- Live null (B − A) residual in dBFS, displayable or audible
- Author loop: edit → rebuild → respawn → swap; music never stops; a crash costs one
  respawn
- Every rebuild auto-nulled against the previous build in exact mode, as a regression
  oracle
- Cost: ~2× CPU for the stage under test

## Mobile / sync

- iOS/Android: own app with sandboxed library; Apple's Music app is not a target
- Music app internals: third-party write access is closed (signed/hashed DB;
  iMazing/WALTR reverse-engineer it per release). Not worth it
- Sync options:
  - Local-network diff push (reuses async I/O, monitor protocol, audio-hash identity keys;
    phone contents = set difference)
  - USB push into the app container via libimobiledevice (LGPL, call-out only)
  - Files app fallback
- Background audio via AVAudioEngine plus the background-audio entitlement; Now Playing /
  CarPlay APIs keep the lock screen, Siri, and car working
- Pulling purchases (decided goal: purchases flow out of Apple's app into the player):
  - Phone: `MPMediaItem.assetURL` + `AVAssetExportSession` passthrough for downloaded,
    non-DRM items (nil for Apple Music subscription and cloud-only items). No background
    notifications, so scan on foreground or background refresh by comparing library
    last-modified dates. Retag from MPMediaItem properties; `persistentID` / store ID as
    identity keys. Purchase files embed the buyer's name and Apple ID
  - Desktop: watch Music's media folder with automatic downloads enabled. Plain file
    watching, already tagged
- App Store: Apache/MIT is fine; GPL conflicts

## OS integration

- Media keys / Now Playing: MPNowPlayingInfoCenter, SMTC, MPRIS; Bluetooth headphone
  buttons
- File associations (UTIs, registry, `.desktop`/mime); single-instance handoff; CLI
  enqueue; drag-and-drop
- Distribution: Authenticode signing (SmartScreen), notarization, Flatpak (sandbox fights
  filesystem access and exclusive device access)
- Long uptime (days): leaks matter more than in games

## Smaller features

- Scrobbling: ListenBrainz, Last.fm
- Playlists: M3U8 (relative paths), PLS, XSPF, CUE-as-playlist, iTunes library XML import
- Internet radio: ICY metadata, HLS
- Lyrics: licensing-constrained (LyricFind/Musixmatch); LRCLIB is the open option with
  spotty coverage
- Album art: Cover Art Archive, Discogs; MusicBrainz for metadata

## Testing

- Build a broken-file corpus from day one: truncated frames, lying headers, triple-tagged
  MP3s, mis-encoded CUE sheets
- Null tests end to end (seeded dither makes this possible through the full chain)

## Framing / positioning

- Why this doesn't exist: pieces live in separate communities (DAW plugin isolation in
  Bitwig/AUv3/Reaper; hot reload in Cmajor/Faust; bit-exactness in 3GPP reference codecs
  and vendor DSP tools like SigmaStudio), each solving one pain. Fidelity-focused
  consumers are economically hostile to verification. Most hosts sit on JUCE/Qt/GStreamer
  and don't own threading/process/FP, so determinism can't be retrofitted
- Model is close to Erlang/OTP (isolation, messages, supervisors, hot swap) plus real-time
  scheduling and bit-level determinism
- Korvid ergonomics goal: familiar and friendly. Reach for the familiar thing, get the
  correct thing in a familiar wrapper
  - Wrappers must either truly have familiar semantics or look different exactly where
    they differ
  - Message I/O via coroutines (native in Lua; C++20 coroutines on the native side):
    sequential-looking code that never blocks. The seam to make visible is interleaving at
    yield points
  - Opinions should fail loudly at the boundary with the reason attached
  - Where people repeatedly reach for escape hatches, a builtin is missing
- The player itself (crash-respawn mid-song, live nulls) is the strongest argument for the
  model, as goldberg is for the toolchain
