# Slice 4 notes

What building [slice 4](../PRDs/Blackwing-Slice-4-PRD.md) found, milestone by milestone,
with the numbers. Written as it goes.

## Milestone 1, the window

Nov 23, 2026. Linux laptop (Ryzen 7 7840U, GNOME 50 on Wayland, a 120 Hz panel),
`linux-dev`, Korvid at `slice-11` (52f9963).

**The loop is the frame loop's, paced `Decoupled`** (KR20), and the PRD's open question
is closed that way. `blackwing` with a face calls `frameloop_main` with a command line
of its own making; with none it keeps the loop it had. Both follow one rule, and the
rule is the modules': the player asks for the next tick (`engine_want_ticks`) while it
plays or has anything on its way to the ear or off the disk, the library while it scans
or owes a window an answer, and a loop that hears no one asks ticks four times a second.
The headless loop used to look at the transport and the scan itself; now it reads
`Engine::ticks_wanted`, as the frame loop does.

**Two engines in one executable.** The frame loop starts the engine from the whole
module list, and its own module opens a window: on a machine with no display that is a
failed start. So `play`, `library` and `help` start an engine from a list of their own,
five modules by name (`platform`, `cli`, `player`, `library`, `blackwing`), with the
frame loop, the window, Krome and the face linked and left out. Whether there is a
display is asked before choosing (`DISPLAY` or `WAYLAND_DISPLAY` on Linux): with none,
`blackwing` alone says so and exits 1, and a command given `--face dev` says so and runs
with no face.

**A module for the window, because of names.** Closing the window ends the run:
`ExitRequested` answered with `engine_request_quit`. That handler names an event the
headless engine never registers, so it cannot live in `blackwing`'s own module, which is
in both engines. It is `blackwing_window` (engine name `window`), which also counts
ticks and frames and has the commands the measurements below were made with (`window
status`, `minimize`, `restore`, `stall`).

**The frame loop owns the exit**, memory included, so nothing of `blackwing`'s runs
after it. `--capture-to` writes at exit and is refused with a face. So that a face can
run with no hardware at all, the player now makes the capture device itself when it is
asked for by name and given none (`PlayerConfig::device_backend` null): on the wall
clock, keeping nothing, closed with the player.

**The API's declarations moved into headers** (`blackwing_player/schema.h`,
`blackwing_library/schema.h`): every field table, the event and component tables, the
resources. A stand-in for the player is a module named `player` that names those tables
and publishes values a test sets, and `ui_dev`'s tests run against it with the player
not in the engine (BD4, BD21). With that the three resources got their field tables,
cosmetic (KR17), a milestone early.

**One screen, and a panel is a definition.** Krome's screens are whole-window layers:
the top one takes every press, so panels cannot be screens side by side, and a mistake
anywhere in a screen's files fails the screen. The face is therefore one screen
(`shell.kim`) and each panel a definition in a prelude of its own with its script beside
it. For "a screen that will not load shows its error where it would be, and the other
panels work" (BD18), `ui_dev` reads Krome's errors when the shell fails: each one that
names a panel's file makes that panel a stand-in, a definition of the same name compiled
on the spot (`kim_compile`) that says the file, the line and the message, given to Krome
in the file's place (`krome_provide`), and the shell is opened again. A mistake that
names no panel leaves a screen of the errors. The player is not involved in any of it.
A stand-in stays until the next start: what Krome was given by hand wins over the file.

**A model value is written when it differs.** `ui_dev`'s writer keeps a hash of what each
key last showed; a write of the same value is nothing, and one that goes through is the
only reason the face asks for a frame. The position is written every tick as text, and
the text changes once a second. A `KromeSet` carries 56 bytes, so a longer string goes
as a document (`krome_set_doc`).

**A command needs a tick.** A press is heard in a frame; the command it becomes is acted
on in the next tick, which under decoupled pacing may be a quarter second away when
nothing is playing. The face asks for four ticks after each command it emits, and
`blackwing` for one after its command line's.

### Measured

The capture device on the wall clock, a two-minute FLAC, a window of 1280 x 800 on the
Vulkan renderer. Counts are `window status` over ten seconds.

| state | ticks | frames drawn |
|---|---|---|
| stopped | 40 | 0 |
| paused | 40 | 0 |
| playing, the position showing | 601 | 20 |
| playing, minimized (X11) | 599 | 0 |

- **Still is free of frames and layouts** (BD3): paused or stopped, none in ten seconds,
  in the window and in the test (which also counts no model write and no request for a
  tick or a frame).
- **The position costs two frames a second**: ten model writes, ten layouts and ten
  paints in ten seconds, against 600 ticks, and twenty frames presented. The layout is
  the readout changing width. The second frame of each pair draws nothing new: Krome asks
  for a frame as it hands the renderer a new list, in the frame that is about to draw it,
  and the request outlives that frame. Krome's to look at; not asked for yet.
- **Hidden is asleep** (BD3): minimized under X11, 599 ticks and no frame; the face's own
  system passes over every one of them (the test: no table read, no model write); the
  lead stayed at 5.00 s, with no underrun. Shown again, everything it missed is written
  in the next tick.
- **Stalled is not heard** (BD22): the main thread held for 2 s, then 4 s, while
  playing: no underrun, the position on time after. The player's tests hold it for 2 s
  against a wall-clock device and check that two seconds played.
- **A still window is not free of wakeups.** Decoupled pacing with nothing asked sleeps 4
  ms at a time and polls the window: about 230 wakeups a second and 2.5% of a core in
  this build (a 5 s run against a 25 s one), where the headless loop costs 0.03%. Asked
  of Korvid as KR26.
- Start to a window that answers, and memory with the face up: at the close.

**Not done here:** a window minimized by hand on Wayland (Korvid has not checked it
either), covered windows, and the three disks, which are the close's.
