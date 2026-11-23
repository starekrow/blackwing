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

## Not asked for, and worth knowing

- **`engine_frame` and `platform_pump` were all a custom loop needed.** The comment in
  `engine.h` that shows a loop of one's own was accurate.
- **The realtime allocation check earned its keep**: every test's callback ran under
  it, on a thread the capture device marks.
- **`test_deps`, `private_includes`, `licenses` and source-less modules** all did what
  the manual says, first time.
