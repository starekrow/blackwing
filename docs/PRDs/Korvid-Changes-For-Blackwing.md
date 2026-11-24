# Korvid changes for Blackwing — quick requirements

Nov 22, 2026 · draft, for review on the Korvid side

## What this is

What building Blackwing's slice 1 (milestones 1 to 3) found it needs from the engine, as
requirements Korvid can take, change or refuse. Each says what was run into, what is
asked, and how to tell it is done. The findings themselves are in
[docs/design/slice-1-notes.md](../design/slice-1-notes.md).

Nothing here was changed in Korvid's tree. Where Blackwing has a workaround today, it is
named, so the cost of waiting is clear.

One of these blocked the next milestone: **KR1**. Slice 1's milestone 4 is the PipeWire
backend, and a project cannot link PipeWire today without editing the engine.

## Status

Nov 23, 2026. Korvid's slice 10 (`docs/PRDs/Engine-Slice-10-PRD.md` in the engine's
checkout) took all nine, with these IDs. What Blackwing does with each:

| ID | in Blackwing |
|---|---|
| KR1 | used: `hifi_platform` defines the `pipewire` external with `pkg_config` (slice 1, milestone 4) |
| KR2 | used: every sanitizer run since milestone 3 is `kormake test <module> --sanitize ...` |
| KR3 | used: `blackwing` passes no sink |
| KR4 | used: the player's path limit is gone |
| KR5 | not yet: the player still calls the blocking `io_stat` before a read |
| KR6 | not yet: wanted by slice 2's seeking |
| KR7 | not needed so far: PipeWire's data thread arrives in the realtime class on its own. A backend that owns its thread (ALSA `hw:`) will use it |
| KR8 | used: `hifi_codec` builds `flac.cc` with the `wrapv` profile |
| KR9 | documented on the engine's side; the player's `seq` stays |

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
before any of it is built. Not yet put to Korvid.

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

Nov 23, 2026. Found at slice 3's close; neither blocks it. Not yet put to Korvid.

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
API from a script. Not yet put to Korvid. KR17 and KR18, above, were P1 and now block
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
