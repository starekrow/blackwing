# Korvid changes for Blackwing — quick requirements

Nov 22, 2026 · updated Nov 23 with what Korvid took, with what slice 4 found, and with
what skin import needs

## What this is

What building Blackwing's slice 1 (milestones 1 to 3) found it needs from the engine, as
requirements Korvid can take, change or refuse. Each says what was run into, what is
asked, and how to tell it is done. The findings themselves are in
[docs/design/slice-1-notes.md](../design/slice-1-notes.md).

Each was written before Korvid took it, with Blackwing's workaround named so the cost of
waiting was clear. Korvid's slices 10 and 11 took the first twenty-five: Status has where
each landed and what Blackwing does with it now. KR26 and KR27 are new, from building
slice 4. KR28 to KR42 are new, from the skin import note, and are for work in no phase yet.

One of these blocked the next milestone: **KR1**. Slice 1's milestone 4 is the PipeWire
backend, and a project cannot link PipeWire today without editing the engine.

## Status

Nov 23, 2026. Korvid took all twenty-five, with these IDs: KR1 to KR17 and KR23 in its
slice 10 (`docs/PRDs/Engine-Slice-10-PRD.md` in the engine's checkout, branch
`slice-10`), the rest in its slice 11 (`Engine-Slice-11-PRD.md`, branch `slice-11`,
which starts from `slice-10`). Both are pushed; neither is merged to the engine's `main`.
Each PRD's status has what was built and checked; Windows, the Apple family and the
phones are written there and only partly checked (macOS and the iOS Simulator for some).

| ID | in Korvid | in Blackwing |
|---|---|---|
| KR1 | externals in a `module.lua` or `korvid.config`, `pkg_config` | used: `hifi_platform` defines the `pipewire` external (slice 1, milestone 4) |
| KR2 | `--sanitize address,undefined` and `thread` | used: every sanitizer run since milestone 3 |
| KR3 | a null sink discards | used: `blackwing` passes no sink |
| KR4 | any path the OS takes | used: the player's path limit is gone |
| KR5 | `IoQueue::stat_file` | used: the player stats a track through the queue |
| KR6 | `IoRequest::offset` and `length` | used: the player's pieces, the library's tag reads |
| KR7 | `os_thread_enter_audio_class` | not needed so far: PipeWire's data thread arrives in the realtime class on its own |
| KR8 | `codegen_sources` and the `wrapv` profile | used: `hifi_codec`'s `flac.cc` |
| KR9 | documented: order is per type | the player's `seq` stays |
| KR10 | `os_cond_timed_wait` | used by the library; `hifi`'s render thread still naps (`hifi.cc`) |
| KR11 | `io_list_dir` | used: the library's walk and its watcher |
| KR12 | `io_path_is_remote`; a watcher made with `Auto` polls a share's root, and `add_root(dir, Poll)` | used: the library's watcher polls by root |
| KR13 | `IoStat::volume` | used: the library's walk |
| KR14 | `.c` sources, with a toolchain's `cc` | used: `blackwing_library/src/sqlite.c` |
| KR15 | `io_make_dirs` and `io_remove_tree` take any length | nothing to change but the corpus test's `mkdir`, which may go |
| KR16 | a recursive watch neither follows nor reports a link to a directory | the library's filter of paths through a link (`watch.cc`, `Links`) may go |
| KR17 | `ResourceDesc::cosmetic` | used: `player.transport`, `player.output` and `library.scan` have field tables (`schema.h`, slice 4 milestone 1) |
| KR18 | `script_push_events`, `EventTypeDesc::defaults`, `script_push_resource` | used: `blackwing_script` is built on all three (slice 4, milestone 5) |
| KR19 | `engine_add_event_type`, `engine_add_ktable`, their removal, `engine_listen` | `engine_listen` used by the script host's walk, to count every event type with no handler written; the rest not yet: B33 |
| KR20 | `Pacing::Decoupled`, `engine_want_ticks` | used: `blackwing` with a face is `frameloop_main` paced `Decoupled`; the player and the library ask for their ticks, and the headless loop reads the same request |
| KR21 | `WindowVisibility`, `HostWindow::visible()`; no frame drawn for a hidden window | used: `ui_dev` sleeps on it; minimized under X11, 599 ticks and no frame in ten seconds. A window minimized by hand on Wayland is not checked |
| KR22 | `each row in rows height=24 count=@total from=@first -> view`, a `placeholder` block, `KromeView` | used: `ui_dev`'s Queue panel (slice 4, milestone 2). The check does not know `placeholder` (KR27) |
| KR23 | `engine_set_event_hook`, dev builds | used: `ui_dev`'s Events panel keeps what the hook is handed (slice 4, milestone 4) |
| KR24 | `platform_user_dir` (settings, data, cache), and `platform_settings`, a store in `settings.json` | used in part: the session is a file of its own in the settings directory (slice 4, milestone 6). The store is not used yet: a session is not a setting. The index has not moved to the cache directory |
| KR25 | `image/jpeg.h`: baseline, progressive, scaled, EXIF, fuzzed | not yet: BL25 and slice 6 |

| KR26 | not asked of Korvid before: found in slice 4, milestone 1 | the still window polls: about 230 wakeups a second |
| KR27 | not asked of Korvid before: found in slice 4, milestone 2 | worked round: a definition named `placeholder` in `ui_dev/widgets.kim` |
| KR43 | not asked of Korvid before: found in slice 4, milestone 4 | a path typed into the face is refused past 510 bytes, with a line beside the edit |
| KR44 | not asked of Korvid before: found in slice 4, milestone 6 | the window comes back at its size and not its place; a list comes back at its top |
| KR45 | not asked of Korvid before: found at slice 4's close | nothing: a present that blocks holds the player's ticks |
| KR28 to KR42 | not asked of Korvid before: from the skin import note | nothing built: skin import is in no phase |

Where Korvid's answer differs from what was asked:

- **KR16**: a link to a directory is not reported at all, rather than reported with a mark
  saying it came through one; nothing asked for following, so there is no option for it.
- **KR19**: no unit is scheduled against a runtime type. It is emitted from outside the
  schedule (or a main-thread unit) and read with `engine_listen`, which works for any
  type. A run adds at most 32 runtime ktables.
- **KR24**: on macOS and iOS settings sit beside data in `Library/Application Support`
  (Apple asks that nothing but the defaults system's plists go in `Library/Preferences`),
  and the store is a file there, not the defaults system. On Windows settings and data
  share `%APPDATA%`. The store is JSON, so a person can read it.
- **KR25**: written for the engine, not vendored. Its pixels are libjpeg-turbo's (Pillow's)
  exactly on every test case; a 1500 x 1500 cover to an eighth in about 3 ms, a whole decode
  two to four times libjpeg-turbo's time (no SIMD). The fuzzer is a mutation loop in the
  test, not coverage-guided.

Found on the way, in Korvid: the state hash counted a pending timer by its event type's id,
which moves when a module adds a type, so the same sim could hash differently. It now uses
the type's name. Recorded hashes taken before slice 11 do not match after it.

## Requirements

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR1 | A project can name and link an external library without changing the engine | kormake | P0, blocks slice 1 milestone 4 |
| KR2 | A project can build and test under a sanitizer from one command | kormake | P1 |
| KR3 | `cli_run` with no sink does not crash | `cli` | P1 |
| KR4 | The I/O queue takes paths longer than 255 bytes | `platform` | P1 |
| KR5 | A file's size and kind can be asked through the I/O queue, without blocking a tick | `platform` | P1 |
| KR6 | A ranged read of a file through the I/O queue | `platform` | P1, wanted by slice 2 |
| KR7 | A thread can be put in the OS's realtime audio class | `core` | P1, wanted by slice 1 milestone 4 |
| KR8 | A module can set compiler flags for a vendored source, on every toolchain | kormake | P2 |
| KR9 | Events of different types can be read in the order they were emitted | `core` | P2, or documented as not offered |

Two things slice 1's PRD asked for turned out not to be needed, and are withdrawn:

- **A headless loop that sleeps.** `blackwing` carries its own, forty lines over
  `engine_frame` and `platform_pump`. `tool_main` is right for tools as it is.
- **A long-lived thread that is not a job.** `os_thread_start` is all the render thread
  needed. (KR7 is what is left of this.)

## KR1: externals a project defines

**What was run into.** A module names a system library with `external = { sdl3 = ">=3.2" }`,
and "the toolchain defines what each name means": the name must be in
`modules/toolchain-linux/module.lua`'s `externals` table, with its `libs`, and for a
package in a directory its `dir`, `include` and `libdirs`. Blackwing's `hifi_platform`
needs PipeWire on Linux (`libpipewire-0.3`, with headers in two versioned directories
under `/usr/include`), and later WASAPI's and Core Audio's system libraries, sqlite, and
whatever a third-party codec brings. Each would be an edit to an engine toolchain for a
library the engine does not use.

**What is asked.**

1. **A project can define externals**, in `korvid.config` or in the `module.lua` of the
   module that uses them, per platform, with the fields a toolchain's external has
   today: `libs`, `frameworks`, `dir` or `env`, `include`, `libdirs`, `runtime`,
   `check`, `fetch`.
2. **Include directories for a system package** that is not in one directory of its own:
   absolute paths, or a way to ask the system for them. PipeWire's are
   `/usr/include/pipewire-0.3` and `/usr/include/spa-0.2`; `pkg-config --cflags
   libpipewire-0.3` is the portable way to learn that. A `pkg_config = "libpipewire-0.3"`
   field that fills `include`, `libdirs` and `libs` at generation time would cover it,
   with the explicit fields as the fallback where there is no pkg-config.
3. **A missing external is a message, not a compile error**: the module that needs it,
   the package, and the `fetch` line (`sudo apt install libpipewire-0.3-dev`), at
   `kormake check` and before any compile, as for a toolchain's directory externals now.
4. **The engine's own externals are untouched**, and a project may not redefine one: a
   name defined twice is an error that names both places.
5. **The version constraint is checked** where the system can say (pkg-config's
   `--atleast-version`), and otherwise is documentation, as it is today.
6. **`kormake graph` and the host-tools line show a project's externals** beside the
   engine's, so what an executable links is still readable in one place.

**Done when** Blackwing's `hifi_platform` builds against PipeWire on Linux with only
files in Blackwing's tree changed, a machine without the development package gets the
`fetch` line from `kormake check`, and Korvid's own tree builds exactly as before.

**Workaround today.** None inside Blackwing. The alternatives are editing Korvid's Linux
toolchain for each library, or loading the library at run time with `dlopen` and
declaring its functions by hand, which gives up the headers.

## KR2: sanitizer builds for a project

**What was run into.** Blackwing decodes files from anywhere and runs three threads
around a lock-free ring. Each milestone was run under AddressSanitizer,
UndefinedBehaviorSanitizer and ThreadSanitizer, and that found a real overflow in a
vendored decoder. Each time it was done by copying a compile line out of Ninja and
adding flags by hand, linking the engine's libraries uninstrumented.

**What is asked.** `kormake test <module> --sanitize address,undefined` (and `thread`),
building the module, its closure and its tests instrumented, into a build directory of
their own, and running them. Linux first. The engine's realtime-thread allocation check
should keep working under it.

**Done when** one command per sanitizer runs a project's tests instrumented end to end,
engine modules included.

**Workaround today.** The hand-built runs, which cannot instrument the engine's side.

## KR3: `cli_run` with no sink

**What was run into.** `cli_run(e, line, nullptr, nullptr)` crashes in the `cli` module's
`respond` when the command prints. A caller that wants the effect and not the text has
to pass a function that does nothing.

**What is asked.** A null sink discards the lines. A failure still returns false.

**Done when** the call above returns true for a command that prints.

**Workaround today.** A no-op sink, in `modules/blackwing/src/main.cc`.

## KR4: longer paths in the I/O queue

**What was run into.** `IoEntry::path` is 256 bytes, and `read_file` returns a null
handle for a longer path. Music libraries nest artist, album and track names, in UTF-8,
and go past that routinely; the OS allows 4096 on Linux and 32767 on Windows.

**What is asked.** `read_file` and `write_file` take any path the OS takes, without
making every entry pay for the longest.

**Done when** a file under a path of a thousand bytes reads through the queue.

**Workaround today.** The player fails the item with "the path is too long for the I/O
queue". A user will meet this.

## KR5: stat through the queue

**What was run into.** Before reading a file the player needs its size (to refuse one
larger than the read-ahead cap) and whether it exists. `io_stat` answers at once, on the
calling thread, which is the main thread in a tick. On a network share that has gone to
sleep, that is a tick of several seconds.

**What is asked.** A stat request with a completion, like a read: is it a file, its
size, its modified time. The library's scanner (slice 3) will ask this for every file
it walks, so it matters more there than here.

**Done when** a stat of a path on a slow mount does not hold a tick.

**Workaround today.** The blocking `io_stat`, noted on the player's module page.

## KR6: ranged reads

**What was run into.** The queue reads whole files. The player reads a track whole into
memory, capped at 256 MB, and fails a larger one. Slice 2 adds seeking and files too
large to hold; slice 3's tag reader wants the first and last few kilobytes of fifty
thousand files and nothing in between.

**What is asked.** A read of `size` bytes at `offset` in a file, with the same
priorities, budget and completions as a whole-file read. The queue already does this
inside itself for pack entries.

**Done when** the first 64 KB of a large file can be read without the rest.

**Workaround today.** Whole-file reads, and the cap.

## KR7: the realtime audio class for a thread

**What was run into.** `thread_set_realtime` marks a thread so the allocator can catch a
mistake on it; it does not change how the OS schedules the thread. A device backend's
callback thread should be in the OS's audio class: MMCSS "Pro Audio" on Windows, an
audio workgroup on macOS, `SCHED_FIFO` through rtkit on Linux. PipeWire's own thread
arrives that way already; a backend that makes its own thread (ALSA directly, the
capture device on the wall clock) does not.

**What is asked.** One call that puts the calling thread in the platform's realtime
audio class, says whether it worked, and sets the allocator flag with it. Per platform,
in `core` beside `os_thread`.

**Done when** a thread that asks is scheduled ahead of the job pool's workers on each
desktop, and says so when the OS refuses.

**Workaround today.** None needed until a backend owns its thread.

## KR8: flags for a vendored source

**What was run into.** A vendored decoder overflows a signed integer on damaged input.
The fix is to compile that one library with wrapping arithmetic. GCC can be told from
inside the file (`#pragma GCC optimize`); Clang and MSVC cannot, and a module cannot add
a compiler flag. `codegen` profiles are the nearest thing, and they are for speed.

**What is asked.** A module can name extra flags for some of its sources, in the
toolchain's own words per toolchain, or a profile the toolchains agree on (`wrapv`).

**Done when** Blackwing's FLAC decoder is built with defined overflow on all three
desktop toolchains.

**Workaround today.** The GCC pragma. Clang and MSVC builds are unprotected, and have
not been made yet.

## KR9: order across event types

**What was run into.** An event type is a stream, and a handler sees its own type's
batch. A unit that emits `TrackEnded` for one item and then `TrackStarted` for the next,
in one tick, cannot say so: the two handlers run in the schedule's order. A consumer
that keeps "what is playing" from events gets it wrong half the time.

**What is asked.** Either a way to read several types as one ordered batch, or a line in
the events documentation that says order across types is not kept and what to do
instead. The second is enough.

**Done when** the events page says which.

**Workaround today.** The player's notifications carry a `seq` that counts across types,
and its page tells consumers to read the transport resource for what is playing.

## Found in milestone 5

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR10 | A wait on a condition variable with a timeout | `core` | P2 |

**KR10. What was run into.** `hifi`'s render thread, with nothing to play, holds the
device once the device has played out what it was given. Until then it must look again
shortly, and also wake at once for a command. `os_cond_wait` has no timeout, so it naps
2 ms at a time, checking for commands: up to 500 wakeups a second for as long as a lead
plays out (up to five seconds at the end of a queue).

**What is asked.** `os_cond_timed_wait(c, m, seconds)`, returning whether it was
signalled: `pthread_cond_timedwait` on `CLOCK_MONOTONIC`, `SleepConditionVariableSRW`.

**Done when** a thread can wait for a signal or a deadline, whichever comes first.

**Workaround today.** The naps, in `modules/hifi/src/hifi.cc`.

## Found in planning slice 3

Nov 23, 2026. From [Blackwing-Slice-3-PRD.md](Blackwing-Slice-3-PRD.md), the library,
before any of it is built. Taken by Korvid's slice 10.

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR11 | One directory can be listed, not only a whole tree | `platform` | P0, blocks slice 3 milestone 3 |
| KR12 | Whether a path is on a network share can be asked | `platform` | P2 |
| KR13 | A stat says which volume the path is on | `platform` | P2 |
| KR14 | A module's `.c` sources are compiled as C | kormake | P0, blocked slice 3 milestone 3 |

### KR11: listing one directory

**What was run into.** The library's first pass walks a root to list its audio files.
`io_list_files` and `io_list_files_stat` walk the whole tree under a directory in one
call, in name order, skipping hidden entries. A caller can stop the walk and cannot
steer it. The library has to steer: when someone opens a folder that has not been
walked, that folder is walked next, ahead of the rest. On the built-in disk fifty
thousand files list in about a second and it would not matter. On a USB 2 drive or a
share the walk is minutes, and "what you look at is scanned next" is the point of the
design.

**What is asked.**

1. **A listing of one directory's own entries**, not its descendants: for each, its
   name, whether it is a file or a directory, and for a file its size and modification
   time, from the listing itself where the OS gives them (as `io_list_files_stat` does
   now).
2. **Usable from a task** on the I/O queue's workers: blocking is fine, the main thread
   is not where it runs. A request through the queue with a completion would do as
   well, and is not needed.
3. **Hidden entries on request.** Skipped by default, as now.
4. **A link is said to be a link**, as well as what it points at, so a walk can refuse
   to follow one out of its root or round a loop.
5. **A name that is not valid UTF-8 is still listed**, in a form that can be handed
   back to `read_file` and opens the same file. Old music collections have them.
6. **Failure says why**: not there, not a directory, no permission, an I/O error. The
   library marks the directory and goes on, and "offline" is decided from this.

**Done when** a caller can walk a tree breadth first, a directory at a time, in an
order of its own choosing, and a directory of ten thousand entries lists without ten
thousand stats.

**Workaround today.** None through the engine: the walk's callback can stop it, not
skip a subtree, so listing one level of a directory means walking everything under it.
The alternatives are `opendir` in Blackwing, per OS, beside the
engine's own; or walking the whole root in one call and giving up the priority slot for
directories not yet reached.

### KR12: is this path on a share

**What was run into.** The library watches each root with `FsWatcher`. On a network
file system the native backend takes the watch and reports only changes made from this
machine: a file added from another computer is never heard of. The watcher cannot know;
it is the OS that is quiet. Such a root wants the polling backend.

**What is asked.** One call: is this path on a file system whose changes the OS may
not report (NFS, SMB, sshfs and other FUSE mounts on Linux; a remote drive on Windows;
a network volume on macOS)? Or the same knowledge inside `FsWatcher`, as
`FsWatchBackend::Auto` choosing to poll such a root.

**Done when** a root on a share is polled without the user saying it is one.

**Workaround today.** Every root is walked once at start-up and again at an interval
whatever its watcher says, so a missed change is found late, not never. The cost is a
walk that a trusted local root does not need.

### KR13: which volume a path is on

**What was run into.** A music disk that is unplugged leaves its mount point behind as
an empty directory. A library that walks it finds nothing and, believing what it sees,
forgets fifty thousand tracks and their corrections. The library needs to tell "this
is not the disk I scanned" from "this disk is now empty".

**What is asked.** `IoStat` gains an identity for the volume the path is on, stable
while it is mounted and different for another volume at the same path: `st_dev` on
POSIX, the volume serial number on Windows. Comparing two for equality is all it is
for. A file system's own UUID, which would also survive a reboot, would be better and
is not asked.

**Done when** the stat of a mount point differs with the disk mounted and without.

**Workaround today.** A root that is empty where it held tracks is treated as offline,
and files that vanish are marked missing and kept for a month before they are
forgotten. That is right for an unplugged disk and wrong for someone who really did
delete everything; they will see a root marked offline until they remove it.

Nov 23, 2026: KR11 to KR13 were built in Korvid's slice 10 before slice 3 began.

### KR14: C sources

**What was run into.** Slice 3 vendors sqlite's amalgamation, which is C and not C++ (it
uses `new` as a name; g++ stops at 740 errors), and kormake compiled only `.cc` and `.mm`.
No module field or tool rule could add a C object.

**What was done**, in Korvid's tree on its `slice-10` branch with the go-ahead, as the
engine's slice 10 PRD records: `.c` sources in `src/` are a third language with a unity
file of its own, built with the toolchain's `cc`, `cflags` (per config too) and `c` rule,
and the module's includes, defines and profiles. Every desktop toolchain has them (Linux
built and used; the others written). A toolchain with none cannot build such a module and
says which file. `blackwing_library/src/sqlite.c` sets sqlite's options and includes it.

With it, a fix found on the way: a module's heavy tests now take its `test_deps`, and a
test's `test_deps` bring their externals (the library's tests take the player, which takes
PipeWire).

## Found in building slice 3

Nov 23, 2026. Found at slice 3's close; neither blocks it. Taken by Korvid's slice 10.

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR15 | `io_make_dirs` takes a path as long as the platform allows | `platform` | P2 |
| KR16 | `FsWatcher` does not follow links to directories, or says a path came through one | `platform` | P1 |

### KR15: deep directories

**What was run into.** `io_make_dirs` copies its path into a 1024-byte buffer and fails
on anything longer, though Linux allows 4095 and the rest of the I/O layer takes them
(KR4). The library's override files mirror each root by its path inside Blackwing's
data, so a directory whose path is near a thousand bytes under its root cannot have its
overrides saved; the corpus has one. The corpus test makes its deep tree with `mkdir`.

**What is asked.** The same limit as the rest of `io`.

**Workaround today.** None needed for real libraries; an override in such a directory
fails to save, with a message.

### KR16: the watcher and links

**What was run into.** The corpus has a link inside a root that points at the root. The
library's walk never follows a link to a directory (KR11 says which entries are links).
`FsWatcher` does: it watched the tree through the link, again and again down to the
length limit, and reported a file written in the root as
`tree/loop/tree/loop/.../growing.flac` as well, which the library took for a second file.
Each pass round the loop is another set of inotify watches, from a limit of 8192 on older
kernels.

**What is asked.** That a recursive watch does not descend through a link to a
directory, as the walk does not; or, if following is wanted somewhere, an option, and a
way to tell which reported paths came through one.

**Workaround today.** The library drops any event whose path within the root passes
through a link, asking each directory once of its parent's listing. The watches spent on
the loop are still spent.

## Found in drafting the script-module spec

Nov 23, 2026. [The script-module spec](../specs/script-modules.md) is a draft and nothing
of it is built, so none of these blocks anything yet. They are what its binding needs
from the engine, written down while the reasons are fresh. B33, script modules, is P1.

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR17 | A resource can have a field table without becoming sim state | `core` | P0, blocks slice 4 milestones 4 and 5 (was P1, for B33) |
| KR18 | Events can be emitted and read from Lua by their field tables | `script` | P0, blocks slice 4 milestone 5 (was P1, for B33) |
| KR19 | Event types and ktables can be registered after `engine_init` | `core` | P1, wanted by B33 |

### KR17: a cosmetic resource's fields

**What was run into.** A script reads a ktable's rows by field name, from the ktable's
own field table (`script_push_ktable`). A resource has no such thing unless its
`ResourceDesc` sets `fields`, and setting it makes the resource sim state, part of the
state hash, with the published object checked as plain data. `player.transport`,
`player.output` and `library.scan` are cosmetic: they follow a device's clock and a
disk, and must not be hashed. So today they have no field table, and nothing outside C++
can read them by name: not a script, and not `describe`.

**What is asked.** A way to give a resource a field table that describes it without
hashing it: a flag on `ResourceDesc`, as a ktable has `cosmetic`.

**Workaround today.** None in use. The script host could carry its own copy of the three
layouts, which is a second home for a fact and drifts.

### KR18: events in Lua

**What was run into.** `modules/script` binds a ktable to Lua from its field table, with
the type mapping and the errors a script needs. Nothing does the same for events, though
every one of Blackwing's event types carries a field table (for `--trace-events`). The
player's and the library's whole API is events in and events out, so a script that can
read rows and not emit a `PlayerPlay` can do nothing.

**What is asked.** In `script`, beside `script_push_ktable`: emit an event of a named
type from a Lua table of its fields, with fields left out taking the struct's defaults;
and push a batch of a type as a list of tables. The same mapping and the same errors as
a row's: a wrong type, an integer that does not fit, a string too long, an unknown field.

**Workaround today.** None in use. The host could write it over `FieldDesc` itself; it
would be the ktable binding's code again.

### KR19: registering after start-up

**What was run into.** A module's event types, components and ktables are tables in its
`ModuleDesc`, laid out by `engine_init`. A script module is found and loaded while the
engine runs, and the spec lets it declare tables and event types of its own (its
`publish` capability), for faces and other scripts to read. No call registers one after
`engine_init`.

**What is asked.** That an event type and a cosmetic ktable can be registered, and taken
away again, while the engine runs: from a field table built at runtime, never as sim
state, at a phase boundary. A script module is unloaded and replaced mid-song, so what it
registered has to go with it.

**Workaround today.** None in use. The host could own a fixed pool of generic tables and
one generic event type, and hand out pieces; a face would then read a script's rows
through the host and not by name.

**Looked at and not asked for.** The spec's stage and decoder states live inside a fixed
block, load text only and open some of Lua's libraries. `kv_lua_newstate` takes any
`Allocator`, and the rest is the host's to do on its own state, so this needs nothing
from the engine.

## Found in planning slice 4

Nov 23, 2026. From [Blackwing-Slice-4-PRD.md](Blackwing-Slice-4-PRD.md), the development
face, before any of it is built: the first window, the first face, the session, and the
API from a script. Taken by Korvid: KR23 in its slice 10, the rest in slice 11. KR17 and KR18, above, were P1 and now block
this slice: the face's Tables panel, the session and a script's `ctx:resource` all read a
resource by its fields, and a script that cannot emit an event cannot drive anything.

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR20 | A window's loop can run ticks with no frame drawn, and draws only when something to draw changed | `frameloop` | P0, blocks slice 4 milestone 1 |
| KR21 | A window says when it is hidden and when it is shown again | `platform` | P0, blocks slice 4 milestone 1 |
| KR22 | A virtualized list over rows of which the model holds only a window | `krome` | P0, blocks slice 4 milestone 3 |
| KR23 | The event trace can be read as data | `core` | P2 |
| KR24 | Preferences storage on every platform: where settings go, and a small store kept there | `platform` | P1, wanted by slice 4 milestone 6 |
| KR25 | A decent JPEG decoder | `image` | P1, wanted by BL25 and slice 6 |

### KR20: ticks without frames

**What was run into.** The player does its work in ticks: it feeds the audio path its
pieces and publishes the position, at the clock's rate, for as long as something plays.
`frameloop` paced on demand decides one thing per pass, whether to tick and draw, so a
tick comes with a frame and a frame with a tick. A player in such a window has two bad
choices. It can ask for a frame every tick while it plays (`engine_want_frame`), and
then a window nobody is touching is presented sixty times a second. Or it can leave the
loop alone, and then it is ticked four times a second and when I/O is pending, which
plays only as long as the lead holds out. Paced continuously it draws at every vsync,
which is the first choice again.

**What is asked.** A pacing in which the two are separate: a module can ask for ticks at
the clock's rate for as long as it wants them, with no frame drawn or presented, and a
frame is drawn only when something to draw changed or someone asked for one. With no
ticks wanted and nothing changed, the loop sleeps as on-demand pacing does now. Ticks
must go on while the window is hidden (KR21), being resized, or having its title bar
dragged.

Whether this is a third `Pacing`, or `frameloop` offering its window and renderer to a
loop the program owns (as `frameloop_step` nearly does, but it pumps, ticks and renders
as one), is the engine's choice. Blackwing has a loop of its own already and would use
either.

**Done when** a windowed program whose module asks for ticks and changes nothing on
screen runs its Sim at the clock's rate for ten seconds and presents no frame; and one
that asks for neither wakes no more often than on-demand pacing does today.

**Workaround today.** None good. `blackwing` could grow its headless loop a window and a
renderer by doing what `frameloop`'s startup does, which is the mechanical part of every
game written a second time.

### KR21: a hidden window

**What was run into.** The master PRD's B25: a hidden face runs nothing. A face that is
minimized, wholly covered, on another workspace or behind a locked screen should stop
refilling its lists and writing its models, and the loop should stop drawing. Nothing in
the platform's headers says a window is in any of those states, so a face cannot know.

**What is asked.** An event when the window stops being visible and one when it is
visible again, and the state readable in between. As good as each OS can tell: minimized
everywhere; covered or on another workspace where the OS or the compositor says so
(occlusion on macOS and Windows, a suspended surface on Wayland). Where it cannot be
told, the window is taken to be shown.

**Done when** a module is told within a frame or two that its window was minimized and
that it was restored, on each desktop; and the loop draws no frame for a window it knows
is hidden.

**Workaround today.** None. A face that is hidden but changing would go on being laid
out and painted for nobody.

### KR22: a list over a window of rows

**What was run into.** Krome's virtualized `each` builds only the rows in view, from a
list the model holds whole: ten thousand rows in the model, thirty in the tree. The
library's lists are fifty thousand tracks, each row several long strings, and the
library hands them out as windows of at most 512 rows that come back a tick or two after
they are asked for. Holding all fifty thousand in a model is holding the index twice,
and they are not there to hold when the list first draws.

**What is asked.** A virtualized list that is told how many rows there are, and holds
only some of them:

- it lays out and scrolls as if every row were there;
- it says which rows are in view when that changes (an event, with the range), so the
  screen's owner can ask for them;
- the model supplies a run of rows and where the run starts, and may replace it at any
  time;
- a row in view that the model does not hold is drawn from a placeholder block of the
  same height;
- rows replaced under a list at rest change in place: the scroll does not move and the
  rows that did not change are not built again.

Fixed-height rows are enough, as now.

**Done when** a list of fifty thousand rows whose model never holds more than a few
hundred can be dragged from end to end by its scrollbar with no frame longer than a
refresh, showing placeholders where rows have not come.

**Workaround today.** The face does a scroll box's arithmetic itself: a spacer above, the
rows it holds, a spacer below, and a behaviour that watches the scroll. It can be made
to work and is a virtualized list written outside the kernel that has one.

### KR23: the event trace as data

**What was run into.** `ui_dev`'s Events panel shows every event as it passes, with its
type, who emitted it and its fields. `--trace-events` already has all of that, in dev
builds, and prints it as log lines. A module cannot receive it.

**What is asked.** A hook a module can set to be handed each delivered event: its type's
name, its field table, the emitter's name and the bytes. Dev builds only is acceptable.

**Workaround today.** A handler per event type, in the face. It shows the types the face
was written to know, and a type added next slice is missed until someone adds its
handler.

### KR24: preferences storage

**What was run into.** Slice 4 saves the session: the queue, the place, the volume and
the equalizer, the window's size and position, which panel was showing. The engine has
one per-user directory, `platform_pref_path`, documented for Linux and macOS, and it is
the data directory (`$XDG_DATA_HOME`, `Application Support`). Blackwing has three kinds
of thing to keep, and each OS has a different place for each:

| kind | what | Linux | elsewhere |
|---|---|---|---|
| settings | the session, the face chosen, per-device choices | `$XDG_CONFIG_HOME` | `%APPDATA%`; `~/Library/Preferences` or the defaults system; the app's own storage on a phone |
| data | the user's overrides: made by hand, not to be lost | `$XDG_DATA_HOME` | `%APPDATA%`; `Application Support`; backed-up storage on a phone |
| cache | the library's index: rebuilt if deleted | `$XDG_CACHE_HOME` | `%LOCALAPPDATA%`; `~/Library/Caches`; the cache directory a phone may empty |

Today all three go in the one directory. A backup carries a 120 MB index it does not
need, and a user who looks where settings live finds none.

**What is asked.** Two things, of which the first is enough to stop being wrong:

1. **The three places by name**, on every platform Korvid runs on, Windows and the
   phones included: settings, data and cache directories for an organisation and an
   application, made if need be. `platform_pref_path` stays as the data directory.
2. **A small store of settings** kept in the settings place: named values (numbers,
   strings, a struct by its field table as `save` writes one), read at start-up before
   any module needs them, written whole so that a crash or a full disk leaves the old
   store and never half of the new, and written off the main thread. Two programs, or
   two runs, writing the same store must not interleave. Where an OS has a native store
   that tools and backups expect (the defaults system on Apple's), using it is the
   engine's choice; a file in the settings directory is fine.

A queue of ten thousand paths is not a setting, and Blackwing will keep it as a file of
its own in the settings directory, so the store does not have to be sized for it.

**Done when** a program can ask for each of the three directories on Linux, Windows,
macOS, Android and iOS and gets the place that OS's users and backup tools expect; and a
value set, followed by a kill, is the old value or the new one at the next start.

**Workaround today.** The session as a file under `platform_pref_path`, written to a
temporary name and renamed (`io_rename`). Right on no platform, and wrong in the same
way on all of them.

### KR25: a JPEG decoder

**What was run into.** Most album art is JPEG: the picture inside an ID3v2 or FLAC tag,
and `cover.jpg` or `folder.jpg` beside the files. The `image` module reads PNG and PPM.
Slice 3 left art unrecorded (BL25, owed), and no face shows it yet; this is asked ahead
of both so that the decoder is not what art waits for.

**What is asked.** JPEG decoding in `image`, as PNG is there: a plain function over
bytes, with no engine state and no I/O, memory from an allocator. Decent means:

- **Baseline and progressive**, 8 bits, greyscale and YCbCr at every subsampling in
  use. A good share of art on disk is progressive. CMYK and Adobe's YCCK at least
  decode to something recognisable; they turn up in scans.
- **Orientation honoured**, from EXIF, or reported so the caller can.
- **Decoded at a fraction of its size**: a half, a quarter, an eighth, by doing less
  work and not by decoding whole and shrinking. A list of albums wants a hundred
  thumbnails from pictures of three thousand pixels a side.
- **Good upsampling of chroma**, not nearest: art is looked at.
- **Safe on a hostile file.** Art comes from anywhere. Dimensions are checked against a
  ceiling the caller gives before anything is allocated; a truncated or spoiled file
  gives the rows it had or a reason, and never a read past its bytes. Fuzzed.
- **The size without the pixels**: dimensions and components from the header alone, so
  a caller can decide before it decodes.
- **Usable from a task**, with nothing shared between calls.
- **Fast enough to be forgotten**: a 1500 x 1500 cover to a 256-pixel thumbnail in a
  few milliseconds on a laptop core.

Arithmetic coding, 12-bit, lossless JPEG and JPEG 2000 are not asked for. A vendored
decoder under a permissive licence is as good as one written; stb_image's has no
scaled decode, which is the kind of thing "decent" is meant to rule out.

**Done when** a set of real covers (baseline, progressive, each subsampling, rotated by
EXIF, greyscale, CMYK, truncated) decodes to within rounding of a reference decoder at
full size, and at an eighth in a small fraction of the full decode's time; and a fuzzer
has run over it with the sanitizers on.

**Workaround today.** None in use: art is not read. Blackwing could vendor a decoder in
a module of its own, which every other Korvid program that meets a JPEG would then do
again.

## Found in building slice 4

Nov 23, 2026. From [slice 4's notes](../design/slice-4-notes.md), as it is built.

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR26 | A still window's loop waits for something to happen instead of polling for it | `frameloop`, `platform_hid` | P1 |
| KR27 | The markup check accepts a sparse list's `placeholder` block | `kimb` | P1 |
| KR43 | A screen's text can be read back whole, whatever its length | `krome` | P2 |
| KR44 | A window's place can be read and set, and a scroll box's; a user directory can be asked for before the engine starts | `platform_hid`, `krome`, `platform` | P1 |
| KR45 | A present that blocks does not hold the ticks | `frameloop`, the renderers | P0 for slice 5 |

### KR26: a still window that sleeps

**What was run into.** `Pacing::Decoupled` does what KR20 asked: with the player paused
the window draws no frame and ticks four times a second. But between those ticks the
loop sleeps 4 ms at a time and pumps the platform, to see input and I/O: about 230
wakeups a second and 2.5% of a core in a dev build on the Linux laptop, for a window
nobody is touching. `blackwing` with no face, paused, wakes four times a second and
costs 0.03%. A music player sits paused and open for days, on a battery.

**What is asked.** With nothing wanted (no ticks, no frame, no Work, no I/O out), the
loop blocks until the window has an event, a waker has something, a frame falls due
(`engine_want_frame_at`) or the heartbeat is reached, whichever is first: SDL's
`SDL_WaitEventTimeout`, `MsgWaitForMultipleObjects` on Win32, the run loop on Apple's.
The I/O queue's completions and the CLI's sockets need a way to end the wait, or their
own deadline.

**Done when** a windowed program that asks for nothing wakes about as often as its
heartbeat, and a key press is still seen within a frame.

**Workaround today.** None; the cost is paid.

### KR27: the check and `placeholder`

**What was run into.** KR22's sparse list takes a block for the rows it does not hold:
`placeholder { ... }`, a child of the `each`. `kim` parses it and Krome's builder finds
it by its word. `kimb_check` (`modules/kimb/src/kimb.cc`, where it walks an `each`'s
children) takes it for a node and refuses it: "'placeholder' is neither a kernel node
(box, text, image, hit, edit) nor a definition". The cook runs the check, so no `.kim`
with a placeholder can be cooked. Krome's sparse tests give the builder a document made
by hand and never run the check; `kim`'s test only parses.

**What is asked.** The check knows `placeholder` as a child of an `each` with `count=`
or `from=`: its children checked as the each's are, an error for one anywhere else, and
for a second.

**Done when** `each row in rows height=24 count=@n from=@f { text "{row.name}"
placeholder { text "..." } }` cooks, and a `placeholder` outside a sparse `each` is a
mistake with its line.

**Workaround today.** `def placeholder { box { slot } }` in the face's prelude. The
check then sees a definition; the builder takes the block before it looks any
definition up, so the definition is never expanded.

### KR43: a model's text, whole

**What was run into.** A face's C++ reads what was typed into an edit with
`krome_model_text`, which copies into a buffer of 512 bytes inside Krome and cuts a
longer value without saying so. A path typed into `ui_dev` (a file to queue, a root to
add) may be up to 4095 bytes on Linux; a path cut at 511 is the path of something else.
Nothing in an event can carry it either: `KromeEvent` has a number.

**What is asked.** A read into the caller's buffer that says how long the value is
(`krome_model_text(e, screen, key, out, cap)`, returning the whole length as `format`
does), or the length beside the pointer. The same question for `krome_set`, whose 56
bytes are cut the same way.

**Done when** a value of four thousand bytes written to an edit is read back whole, or
the caller is told it was not.

**Workaround today.** `ui_dev` refuses a typed path of 511 bytes or more and says so on
screen. Rows are queued by their tables' paths, which never pass through the model.

### KR45: a present that blocks

**What was run into.** `Pacing::Decoupled` keeps ticks and frames apart in what it
decides, and they are still one thread. Under XWayland, with the window not really on
screen, each present waited about a second (the engine's slice 11 notes saw the same
after a restore). The face asked for a frame a second, the position's digit, so the loop
spent its time in the renderer: 40 ticks ran in ten seconds where the player had asked
for 600, and a track sat "starting" for seconds after play was pressed. Minimized, where
no frame is drawn, it ticked at once. Nothing was heard, because the lead is five
seconds and the file was in memory. A file still being read, on a slow disk, is fed in
ticks.

**What is asked.** That a frame cannot cost ticks for longer than a tick or two: a
present with a deadline that gives up and counts the frame as not drawn; or no frame
begun while the last has not been taken; or the render on a thread of its own. The
engine's to choose.

**Done when** a program that asks for ticks and a frame a second, in a window whose
presents block for a second each, runs its ticks at the clock's rate.

**Workaround today.** None. Blackwing's lead hides a stall of a few seconds.

### KR44: putting a window back

**What was run into.** Slice 4's session brings the application back as it was left:
"the window where it was, at its size, on the same panel, its list at the same place".
Three parts of that cannot be done from outside the engine.

1. **A window has a size and no place.** `HostWindow` says `width()` and `height()`.
   Where the window is, which display it is on and what displays there are cannot be
   asked, and a window cannot be told where to open. Where a desktop does not let a
   program place its window (Wayland), saying so is the answer.
2. **A window cannot be resized after it opens**, and it opens in a module's start-up,
   before any module of the program can say at what size. `blackwing` reads the size out
   of its session file before the engine exists and passes it to the frame loop as
   `--size`.
3. **`platform_user_dir` needs an engine**, so that early read cannot ask where the
   settings directory is. `blackwing` carries a copy of the rule for Linux, and on any
   other OS opens at its own size.
4. **A scroll box's offset cannot be read or set.** It survives a rebuild by id, inside
   Krome. A list cannot be put back where it was left, nor (a lesser thing) scrolled to
   the row that is playing.

**What is asked.**

- The window's position and its display, readable, and settable at open and after
  (`WindowDesc` and `HostWindow`); the displays there are, with their bounds, so a place
  on one that has gone is not used; `false` where the desktop will not have it.
- `HostWindow::resize(w, h)`.
- The three user directories from a call that needs no engine (`os_user_dir` is nearly
  it), so a program can read its own settings before it starts one.
- `krome_scroll(e, screen, id, x, y)` and its reading, or the offset bound to the model.

**Done when** a program that saved its window's rectangle and a list's offset opens with
both as they were, on each desktop that allows it.

**Workaround today.** The size, by the early read; nothing for the rest.

Also seen, not asked for: `krome_set` cuts a string at 56 bytes without a word (a
`KromeSet` holds no more); a caller with a longer one must know to use `krome_set_doc`.
An error, or a note where `krome_set` is declared, would have saved a wrong turn.

Also seen, not asked for: each change to a Krome screen is presented twice under
decoupled pacing. Krome asks for a frame when it hands the renderer a new list, inside
the frame that draws it, and the request is still there when that frame is done.

## Found in the skin import note

Nov 23, 2026. From [the skin import note](../design/skin-import-2026-11-23.md): what
converting other players' skins (`.wal`, `.wmz`, `.vlt`, `.xmpskin`) into Krome skins
needs Krome and the window to be able to draw and do. Nothing of it is built and skin
import is in no phase of the master PRD yet, so none of these blocks anything and none
has a priority beyond "wanted by skin import". They are read against Krome's module page
as it stood on this date.

No workarounds are given. The note decides that the converter does not work around a
missing capability: a skin that needs one waits for it.

| ID | Requirement | Where | Priority |
|---|---|---|---|
| KR28 | An image node draws a region of a file | `krome` | wanted by skin import, all four formats |
| KR29 | A strip of frames, the frame chosen by a bound value or played at a rate | `krome` | wanted by skin import |
| KR30 | A node placed at an offset in its parent, each edge tied to a parent edge or kept in proportion | `krome` | wanted by skin import |
| KR31 | An image stretched, tiled, or stretched with fixed borders | `krome` | wanted by skin import |
| KR32 | Bitmap fonts: glyphs as regions of an image | `krome`, `asset` | wanted by skin import, and by `ui_wsz` |
| KR33 | Hit testing through a mask image: by alpha, by colour, by palette index | `krome` | wanted by skin import |
| KR34 | A look computed from an image's pixels: scaled per channel, hue-shifted, gamma-shifted, within a mask | `krome` | wanted by skin import |
| KR35 | Positions and sizes ease, and a screen's script can ask for a timer | `krome` | wanted by skin import |
| KR36 | A slider whose thumb follows a path, and a radial one | `krome` | wanted by skin import (`.vlt`) |
| KR37 | Text that scrolls when it does not fit | `krome` | wanted by skin import |
| KR38 | A screen opened from a directory of uncooked files at runtime | `krome`, `kim` | wanted by skin import |
| KR39 | A window with no frame, shaped by its image's alpha | the window's modules | wanted by skin import, all four formats |
| KR40 | Several windows up at once, each showing a screen | the window's modules, `krome` | wanted by skin import |
| KR41 | Regions of a screen that drag its window, and grips that resize it | the window's modules, `krome` | wanted by skin import |
| KR42 | Windows that snap to each other and to the screen's edges, and move together | the window's modules | wanted by skin import |

"The window's modules" is `platform_hid` and `frameloop` on Korvid's side and
`blackwing_window` on this one; where each of KR39 to KR42 is cut between them is part
of the answer.

### KR28: a region of an image

**What was run into.** Every format keeps many pictures in one file and names them by
rectangle: VLC's `SubBitmap`, a `.wal` element's `x`, `y`, `w`, `h`, a WMP button
group's states, XMPlay's buttons cut from their panel. Krome's `image` draws a whole
file.

**What is asked.** A source rectangle on an image and on a look's image, in the file's
pixels.

**Done when** a node draws the 20 x 20 at (40, 0) of a sheet, and its `hover` look draws
the 20 x 20 beside it, from one texture.

### KR29: a strip of frames

**What was run into.** A knob in XMPlay is any number of states stacked vertically, the
state picked by the control's value. VLC's `nbframes`, `fps` and `loop` play a strip as
an animation. WMP's `CUSTOMSLIDER` and a `.wal` `animatedlayer` are the same two uses.

**What is asked.** An image that is N frames of a strip, down or across: the frame bound
to a model value (`0..1` or an index), or played at a rate for a number of loops. A
playing strip wakes an on-demand window only for its next frame, and a still one costs
nothing (B25).

**Done when** a 64-state knob follows a bound value, and a 12-frame strip at 10 frames a
second plays twice and rests on its last frame with the window asleep after it.

### KR30: placement by offset and edge

**What was run into.** These skins place by coordinate. VLC ties a control's two
corners each to a corner of its container (`lefttop`, `rightbottom`) or keeps a
proportion (`xkeepratio`); a `.wal` measures `x`, `y`, `w`, `h` each from the near or
far edge (`relat*`). A Krome stack places a child at one of nine anchors.

**What is asked.** In a stack, a child's offset from its anchor, and each of its four
edges tied to a parent edge at a distance, or to a fraction of the parent. Offsets can be
bound to the model (KR36 needs that).

**Done when** a node 10 from the left and 10 from the right of its parent stretches with
it, one 30 from the right and 8 from the bottom keeps its size and follows the corner,
and one at a third of the width stays at a third.

### KR31: stretched, tiled and bordered images

**What was run into.** A resizable skin window is a picture made to fit: VLC's `resize`
is `mosaic` (tiled) or `scale`; XMPlay's info window grows in the bands its mask marks
(indexes 253 to 255) between corners that do not; `.wal` frames are the same in nine
pieces.

**What is asked.** An image's fit: natural, stretched, tiled, or stretched with fixed
borders on any of its four sides (the nine-slice `sprite_ui` has for sprites).

**Done when** one frame image with 8-pixel borders draws at 200 x 100 and at 640 x 480
with its corners unscaled, and a tiled background repeats from its top left.

### KR32: bitmap fonts

**What was run into.** Classic and modern Winamp skins and VLC's draw their time
display and often their titles from a picture of glyphs: fixed cells in a grid, in a
stated order (VLC's `BitmapFont` of `type` `digits` or `text`). Krome's fonts are
TrueType from the asset module.

**What is asked.** A font that is an image, a cell size and the characters it holds in
order, used by a text node as any font is. A character it lacks draws as a stated
fallback glyph, or nothing, by the font's choice.

**Done when** a time display draws `12:34` from a strip of digits, and a title with a
character the font lacks still lays out.

### KR33: hit testing through a mask

**What was run into.** These skins are not made of rectangles. A WMP button group has a
`mappingImage` with each button's area filled in one colour. An XMPlay panel has an
8-bit mask whose palette index names the control under each pixel. Everywhere, a round
button should not take a press at its transparent corner. Krome hits a node's rect.

**What is asked.** A node hit only where a mask says: where its own image's alpha is
over a threshold; or where a mask image has a given colour, or a given palette index.
One mask shared by many nodes is held once.

**Done when** two buttons whose rects overlap take presses by the mask's colours, and a
press on a button's transparent corner goes to what is under it.

### KR34: a look computed from pixels

**What was run into.** XMPlay has no hover or pressed art: it multiplies the panel's own
pixels under a control's mask by `scaler_lit` and `scaler_pressed`, RGB factors usually
above 1. WMP images take `hueShift` and `saturation`. A `.wal` colour theme shifts the
bitmaps of a gamma group. Krome has `tint`, a multiply by a colour of at most 1, over a
node's whole rect.

**What is asked.** On a look: a per-channel scale that can exceed 1, a hue shift and a
saturation, applied to the node's image and confined to its mask (KR33) where it has
one. A theme token can carry one, so a colour theme is a theme.

**Done when** a masked control brightens by 1.2 on hover with no second image, and
switching theme recolours every image of a group.

### KR35: easing of place and size, and timers

**What was run into.** A `.wal` drawer slides open; a WMP view's `ontimer` fires every
`timerInterval` milliseconds to move or blink something. Krome's `transition` eases
colours and opacity, and says "sizes and positions do not move"; a screen's script has
no clock.

**What is asked.** `transition` covers a node's size and its offsets (KR30), and says
when it arrives. A screen's script can ask to be called once after a delay, or at an
interval, on the Frame cycle's clock; a timer wakes an on-demand window as a caret's
blink does, and a hidden window's timers do not run (B25).

**Done when** a panel bound to `open` slides 120 pixels in 0.2 s and its handler is told
at the end, and a 500 ms timer costs two wakeups a second and none while hidden.

### KR36: sliders along a path, and radial ones

**What was run into.** A VLC `Slider` runs its thumb along Bezier `points`, not a line,
and a `RadialSlider` picks one of `nbimages` by angle between `minangle` and `maxangle`.

**What is asked.** Perhaps nothing new in the kernel: with bound offsets (KR30) and
frames (KR29), both are Lua behaviours, and std could carry them. Asked for is that the
two are checked to be writable that way, and whichever part is not is named.

**Done when** std (or a prelude of Blackwing's) has a slider whose thumb follows a
three-point curve and a dial of 32 frames, with the keys a slider takes.

### KR37: text that scrolls

**What was run into.** A title longer than its box scrolls: Winamp's song ticker, VLC's
`scrolling="auto"`, XMPlay's title (smooth or by block, `pos_titlescroll`). Krome clips.

**What is asked.** A text node that, when its one line does not fit, moves it: at a
rate, smoothly or by a step, looping or bouncing; still when it fits; draggable by the
pointer where the skin says so. It wakes the window only while it moves and it is in
view.

**Done when** a long title scrolls in a 120-pixel box at a stated rate, a short one sits
still and costs nothing, and the window sleeps when the text is hidden.

### KR38: a screen from a directory, uncooked

**What was run into.** A converted skin is a directory a user has, not an asset in the
pack: `.kim`, images, Lua, fonts. Krome reads `.kimb` through the asset module or from
`krome_provide`, and `.kim` becomes `.kimb` in `kimc` or goldberg's cook.

**What is asked.** A way for a program to open a screen from a directory at runtime: the
markup compiled in process (`kim` and `kimb` as a library call, with the check's errors
by file and line), and its images, fonts and scripts resolved inside that directory and
nowhere else. Reload works on it as on a cooked screen.

**Done when** a skin directory dropped beside a running Blackwing opens, a path that
climbs out of it is refused with its name, and saving its `.kim` rebuilds the screen.

### KR39: a shaped window with no frame

**What was run into.** None of these skins sits in a system window. Each is its own
outline: XMPlay by alpha or a see-through colour, WMP by `transparencyColor` and
`clippingColor`, VLC by `alphacolor` or a PNG's alpha.

**What is asked.** A window with no decorations whose shape is what its content's alpha
says: drawn with per-pixel alpha where the OS composites, and input passing through
where it is clear. What each platform can and cannot do (Wayland, X11 with and without a
compositor, Win32, macOS) is stated.

**Done when** a round skin shows the desktop at its corners, and a click there reaches
the window behind.

### KR40: several windows

**What was run into.** A VLC theme is several `Window`s (main, playlist, equalizer)
shown and hidden by actions; `.wal` containers are the same; XMPlay's panels slide out
of the main one and its info window stands apart.

**What is asked.** More than one window up at once, each showing a Krome screen or
layer, opened, closed, shown and hidden at runtime; one keyboard focus across them; each
paced on demand on its own.

**Done when** a main window and a playlist window are up, one is hidden and shown again
with its scroll kept, and neither draws while the other changes.

### KR41: drag regions and resize grips

**What was run into.** With no frame (KR39) the skin says where the window is grabbed:
VLC's `Image action="move"` and `resizeSE`, XMPlay's "Heading" mask index, a `.wal`
layer with `move="1"` or `resize="bottomright"`.

**What is asked.** A node flag that makes a press on it drag its window, and one that
makes it a resize grip for an edge or corner, through the OS's own move and resize where
there is one (Wayland allows no other way). The window's size bounds come from the
screen (VLC's `minwidth` to `maxheight`).

**Done when** a skin with no frame is dragged by its title area and resized by its
corner between its bounds, on each desktop platform.

### KR42: windows that snap

**What was run into.** VLC's `Anchor`s make windows stick to each other within a `range`
and move together, the `priority` saying which leads; a theme's `magnet` snaps a window
to the screen's edges. Winamp's windows dock the same way, and XMPlay's panels are
attached.

**What is asked.** A window can be told where it is and moved by the program, so
snapping and following are the program's to do; and what each platform refuses is
stated (Wayland gives a client no global position, so there this may be one window drawn
as several, or not offered).

**Done when** a playlist window dragged within 10 pixels of the main one snaps to it and
then follows it, where the platform allows, and the note on each platform says what was
done instead where it does not.

## Not asked for, and worth knowing

- **`engine_frame` and `platform_pump` were all a custom loop needed.** The comment in
  `engine.h` that shows a loop of one's own was accurate.
- **The realtime allocation check earned its keep**: every test's callback ran under
  it, on a thread the capture device marks.
- **`test_deps`, `private_includes`, `licenses` and source-less modules** all did what
  the manual says, first time.
- **`platform_pref_path` is already the per-user data directory** the library needs for
  its index and the user's overrides. Slice 3's first draft asked for one; it was there.
  The index is a cache and would sit better under the OS's cache directory
  (`$XDG_CACHE_HOME`), which the engine does not name. Asked for since, in KR24.
- **`frameloop` links sprites, scenes and a touchpad** into anything that wants a window.
  A music player uses none of them. Not in the way, and not asked for.
- **A face's Lua cannot read a table.** Krome's Lua state is its own by decision, and
  only events cross it, so every screen of a face has C++ beside it carrying rows into
  its model. Slice 4's `ui_dev` will show what that costs before anything is asked.
- **Text.** A music library's titles are in every script, and Krome draws left to right
  with no shaping. Slice 4 gathers a list of real titles and what each looks like; that
  list, not a requirement here, is where the conversation starts.
