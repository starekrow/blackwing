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

## Milestone 2, the player's panels

Nov 23, 2026. The same machine and build.

**As planned:** Transport, Queue, Output and Chain, against a stand-in and then the
player, with the API's changes for parameters and presets.

**What the API was missing**, found by writing the panels, and fixed in the player:

- **A parameter's range, step and unit** (BD13). `StageParam` gains `step` and `unit`,
  and the player publishes `player.params`: a row per parameter of every stage, with
  what it may be and the value heard. A parameter that is 0 to 1 in steps of 1 is a
  switch.
- **Presets as data, and a command to apply one** (BD14). `StageDesc` gains `presets` (a
  name and a value per parameter), the equalizer fills it from the table it had, the
  player publishes `player.presets` and takes `PlayerApplyPreset`. `player eq preset` is
  now that command, and `player.cc` no longer names a band.
- **The device's word, in bits.** `player.output` had `sample`, a `HifiSampleFormat`,
  which a face that includes no `hifi` header cannot read; and "the device's word is
  narrower than the file's" cannot be said without it. It gains `bits` and `float_word`.
- **Not fixed, noted: a table has no way to say it changed.** The Queue panel looks at
  every row's `pos` each tick to find the thirty in view. At four thousand rows that is
  four thousand cache lines sixty times a second while playing. A generation on the
  table, or on the transport, would make it one compare. Left until it is measured at
  the close.

**The chain's controls are written from the table.** A list in Krome is rebuilt when its
model value changes, and a parameter's row changes whenever its value is heard, which is
all the time while its slider is dragged: the node under the pointer would be replaced
mid-drag. So the Chain panel's rows are not a list. `ui_dev` writes a definition as
markup from `player.params` and `player.presets` (a slider a parameter, a switch for one
that is 0 or 1, a button a preset), compiles it and gives it to Krome in the place of a
file that says only "no chain yet". Each parameter has two model keys by its stage and
index: the value heard as text, and where the knob is. A parameter added while it runs
changes the tables' signature and the definition is written again; the test adds one and
finds its slider. This is "any parameter by its own description" (BD6), and the face
names no stage.

**A slider that says so.** std's slider writes the model and emits nothing, and the
face's C++ hears only events. `dev-slider` (`widgets.lua`, thirty lines) is std's over 0
to 1 that emits `slide` with its id as it moves and `slide_end` when let go. While a
slider is held its knob is the pointer's and the face leaves its key alone; the readout
beside it goes on showing the value heard (the exit criterion's "the value heard, not
the value asked for"). The bar in Transport is the same widget: a press or a drag is a
`PlayerSeek` each time it moves, and while a seek is on its way the bar stays where it
was sent.

**The queue is Krome's sparse list** (KR22): it lays out a thousand rows, says which
sixty-odd it has built, and the face hands over those from the table, found by `pos` in
one pass. A row that changes under it changes in place.

**The handler reads the tables early.** Turning "move this row up" into a command needs
the queue (which row is above). The handler emits what the player's pump reads, so it
runs before the pump; the pump writes the table, so a plain read would run after it: a
cycle, and the engine said so. The handler reads `^player.queue` and `^player.params`,
the tables as the phase found them, which is what the screen was showing when the
button was pressed. A resource cannot be read early, so the bar's length is the one the
face last drew it against.

**Found in Krome:**

- **The check does not know `placeholder`.** A sparse list's block for a row it does not
  hold is written `placeholder { ... }` inside the `each`. The builder knows the word;
  `kimb_check` walks an each's children as nodes and refuses it, so the cook fails any
  file that has one (Krome's own tests hand the builder a document and never check it).
  Worked round with a definition of that name in `widgets.kim`, which the check accepts
  and the builder never reaches. KR27.
- **`krome_set` carries 56 bytes**, silently cut. The face's writer sends anything longer
  as a document; the test first typed a path with it and got a file that was not there.
- **A screen's files are checked when cooked**, each `use` included, so a prelude written
  at run time needs a file to stand in its place: `chain_stages.kim`.

**Tests** (`ui_dev`, 123 checks): against the stand-in, every transport command from its
own control; the bar pressed, dragged and let go with a seek on its way; a thousand rows,
scrolled, a row changing in place, each edit from a row's buttons, a path added; exact
and each reason why not; the chain's controls from the descriptions, a slider dragged
with its knob held and its readout heard, the switch, a preset, a parameter added while
it runs; still and hidden as before, with every panel up. Then with no stand-in, the
player itself on a capture device: a path typed into the edit a key at a time and
entered, played, the volume dragged until the readout says what is heard and Output says
why it is not exact, a preset, mute, a press on the bar, pause, play to the end, the row
removed. The test emits no command. Under `address,undefined` too.

**The position, with a bar**: 49 writes, layouts and paints in ten seconds of a track of
4:05 across a bar 944 pixels wide: ten for the readout, 39 for the bar's pixels. In the
window, a two-minute track drew about 27 frames a second: its bar moves eight pixels a
second, and each change is presented twice (milestone 1). This is what "the bar's
pixel" costs; the close decides whether it is too much.

## Milestone 3, the library's panels

Nov 23, 2026. The same machine and build.

**As planned:** the Library panel (roots, every view, search, sort, a row of any kind
opened, queued or played, a field edited), the fifty thousand on screen, titles in the
queue, and the three changes to the API the PRD named. Three of its open questions are
closed here.

**The long path: a longer field** (BD16). Every field that holds a path is 4096 bytes:
`PlayerEnqueue`, `PlayerItem`, `LibraryAddRoot`, `LibraryRoot`, `LibraryPrioritize`,
`LibraryRow` and the two new events (`kPlayerPathMax`, `kLibraryPathMax`; the two modules
do not share a header). That is Linux's limit, which Windows and macOS users will not
reach. The other shape, a table of paths and an id, would have been a third thing every
customer joins against to learn what a row is; a longer field is nothing new to learn. A
row's path is its last field, so the rest of a row stays in its first few hundred bytes.
The cost is memory when the tables are full: 17 MB for a queue of 4,096 rows and 23 MB
for 4,096 library rows, committed as rows are made. The corpus's one track "too long to
play" is a row with its path, and the library's test checks it is a file. `blackwing`'s
command line no longer goes through the engine's (which cut at a thousand bytes and
could not quote a quote): its files and directories are emitted as events.

**Queueing a whole row: the library answers with paths, and glue forwards them** (BD15).
`LibraryCollect{id, kind, row, text}` asks for every track of a directory (with
everything under it), an artist, an album, a genre, a track or a search, in its own
order; the answer is `LibraryCollected`, a path each, sixty-four a tick. The reader
answers it with the windows any face would open, so no new SQL. The library never says
"queue" and the player never hears of a library: `blackwing_glue` turns each path into a
`PlayerEnqueue`. Whoever asks decides what it is for. To play a row the face empties the
queue, collects under an id of its own, and emits `PlayerPlay` when the last path of that
id has come; a script does the same.

**The glue is a module now**, `blackwing_glue`, not `blackwing`'s own handlers: the test
that drives the player and the library through the face needs them joined, and
`main.cc` cannot be linked into a test. It has the handler it had (a file queued is
scanned next) and the new one.

**A queue row's title: a window over paths** (BD12). `LibraryPath{id, index, path}` makes
window `id` a window over paths; its row `index` is what the library knows of the file
there, and a path it does not have is a row with the file's name and a reason. The face
sends one for each queue row in view and the transport's, when the view has been still
for three ticks, and thereafter only for an index whose path changed. The window is
refilled as any is, so a title appears when its tags are read, which for a queued file is
next (the glue). A row is shown under the library's title only if the answer's file name
is the row's: the queue moves under the answers.

**The fifty thousand, on screen** (BD8). The Library panel's list is Krome's sparse one.
The face asks the library for where the list is, half as wide again each side, and asks
again only when the list leaves what was asked: a wheel's notch asks for nothing. Rows
of another list still in the table are not shown as this one's. In the test a stand-in
library of fifty thousand tracks answers two frames late and the scrollbar is dragged
from end to end in 180 frames: **the longest frame 1.6 ms** (Krome's build and layout,
no renderer), placeholders on every frame of the drag, and the rows in view filled one
frame after it stops. A row that changes under the list at rest changes in place.

**What is in view is prioritized** (BD9): a list of tracks at rest for 0.3 s is
`LibraryPrioritize` for its window, once; a folder opened is prioritized as a directory
at once. Hidden, the face closes both its windows; seen again, it asks again.

**Found in the API, and left:**

- **An artist's tracks with no album are in no view.** The Albums view of an artist lists
  albums, and a track with no album tag has none. Collecting an artist collects what the
  face shows under it, so such tracks are neither shown nor queued by artist. They are
  in Tracks, Folders and Search. The library's to settle (with BL23, perhaps).
- **"Every track" is no row's**, so `Queue all` does nothing there. Deliberate: it is
  fifty thousand paths.

**Found in Krome:** a text on one line is as wide as its words whatever room its row
gives it, and `clip` on the text does not cut it: a long path ran across the next three
cells. A cell is now a box of the cell's width that clips, with the text inside
(`dev-text`). This is "no title breaks a row"; the titles in other scripts are the
close's.

**Tests.** The library: a window over paths (found, not under a root, no track there, an
index replaced and one taken out, an override showing, closed) and a collect of each
kind against the views' own order (33 checks). The face against stand-ins for both
(`standin.cc` grew a library): roots with a reason, every view opened from a row, the
window not shown stale, prioritize at rest, a row queued and played, play waiting for the
last path, a field edited and given back, sort, search typed a key at a time, a root
added and removed, titles in the queue and not under the wrong file, hidden and seen
again, and the fifty thousand. Then the player, the library and the glue themselves: a
root typed in and scanned, browsed by folder, artist, album and genre, a search, the one
found played and its title shown from the library, the title corrected by hand and seen
in both lists, an album queued from its row, the queue cleared, the root removed. 224
checks in `ui_dev`, 210 in the library; clean under `address,undefined` and `thread`.

## Milestone 4, the inspectors

Nov 23, 2026. The same machine and build.

**As planned:** Scan, Events and Tables, in tabs beside the Chain under Output, and every
failure given its place.

**Events are the engine's trace, as data** (KR23). The face sets the engine's one event
hook and keeps the last 512 events of the player's and the library's (by their names'
first word: `Player`, `Library`, `Hifi`) in a ring, each as `engine_describe_event`
writes it, with who emitted it. No handler is written for any type, and one added next
slice shows. Live, the list is as many of the newest as fit, newest last; held, nothing
more is kept and the whole ring can be read back; a filter keeps the lines that have the
words typed. Nothing is kept while the tab is not up or the window is hidden. The open
question is closed: it shows commands too, since a hook sees whatever is delivered. In a
ship build the hook does not exist and the panel says so.

**Tables are lines.** A table or a resource is shown from the field table the engine holds
for it: a resource a line a field, a table a line that says which row and then a line a
field, in Krome's sparse list. A field's value is formatted by its type alone. The test
walks the schema's own tables (`k_player_item_fields` and the others) and finds a line
for every field by its name, which is the proof that one added would appear. A new
*table* needs its name in two places (the panel's list and the unit's `reads`): a unit
reads what it declared, and that is the engine's rule, not a gap.

**Found by watching: the slot never emptied.** `library.scan`'s `slot` was set by every
`LibraryPrioritize` and cleared by nothing, so the Scan panel said "something looked at
is being scanned first" for the rest of the run. It has been wrong since slice 3 and no
test read it. It is now a state the threads share: asked for, handed to the tagger,
and empty again when the tagger has read the last of what was handed over. This is the
first bug the face found in what it watches.

**Found in Krome: a text read back is 511 bytes.** `krome_model_text` copies into a
512-byte buffer and cuts a longer value without a word. A path typed into the face
therefore cannot be longer than 510 bytes: one that fills the buffer may be the front of
something longer, and a path cut short is another file's. The face refuses it and says
so beside the edit ("a path typed here can be 510 bytes; a longer one is queued from its
row in the library"). A row's path never goes through the model, so BD16 holds as
written: a track at any path the OS allows is queued from its row. Asked of Korvid as
KR43.

**Every failure has a place** (BD17), each tested against the stand-ins:

| failure | where it shows |
|---|---|
| a file that will not decode | its queue row: `failed`, in red, with the reason |
| a file that fails part-way | its queue row: `damaged`, with how |
| a file whose tags cannot be read | its library row, with the reason |
| a file or a root gone | its library row: `missing`, or `its root is offline` |
| a directory that cannot be listed | its root's counts: `N not listed` |
| an offline root | the root's line: `offline`, and why |
| a rebuilt index | the Library panel's status line, and Scan's index line |
| an underrun | Output's count (and the event, in Events) |
| no device | Output: `no device open` |
| a path too long to take | beside the edit it was typed in |
| a panel that will not load | where the panel would be: the file, the line, the message |

Not shown anywhere, because the library does not publish it: an override file that would
not parse and was set aside. It is a warning in the log. A line in `library.roots` or
`library.scan` would give it a place; left for the library.

**The face from the command line.** `face press ID`, `face show KEY` and `face stats`
(model writes, commands, layouts, paints) on the engine's command line, so a real window
can be driven and asked what it shows by a script. The screenshots and counts in these
notes were taken that way.

**Tests** (`ui_dev`, 262 checks): Scan from the resource; Events with three events
emitted and found by their described text, the filter, hold, clear, and no frame while
nothing passes; Tables for a resource and a table, every schema field found by name, a
value following its source; a damaged row; the typed path refused. The library's slot
test now reads the flag: set while it has work, empty after, and empty when asked for
with nothing to read.

## Milestone 5, scripts

Nov 23, 2026. The same machine and build.

**As planned:** `blackwing_script`, the host's first piece; `blackwing run FILE.lua`; the
conformance walk; the scripted pass. KR19 was not needed, as the PRD expected.

**The binding is the engine's, whole** (KR18, KR17). The host describes no event, row or
resource: `script_push_events`, `script_push_ktable` and `script_push_resource` make all
three from the field tables. What the host adds is the Lua state, the surface as `kv`
shows it, the spec's `ctx` and record, and one unit.

**The unit's declaration is made by the compiler.** `script.slice` reads every event
type, table and resource of the two APIs, and a unit's `reads` is a string in a
`constinit` table. It is put together from the schemas' own tables in a constant
expression (`k_reads`), so a type added to a schema is read by scripts with no edit
here. The schema tables became `constexpr` for that.

**Emits go out of the schedule.** The unit keeps two of the engine's events objects: one
pointed at its own context, to read a batch; one with no context, to emit. The second
goes through the engine's writer for code outside the schedule, delivered at the next
tick's start, which is the spec's rule for a slice's commands; and the unit need not
declare that it emits every command.

**What a field table does not say is beside it.** Which events are commands, the enums'
values by name, which tables there are: the spec had these in a `script.lua` beside each
`module.lua`. They are C++ in each `schema.h` instead. A headless engine mounts no
assets, so the Lua file would not be found; and a `static_assert` holds each enum's
names to its `Count`, which a Lua file could not. The spec is corrected.

**`ctx` and the record's checks are Lua**, three hundred lines run before the script
(`prelude.h`): a unit reaches what it named, a table and a resource that are not the
script's are read-only, a record is checked when it loads.

**Found by building: a system's batch.** The spec said a unit sees "the events emitted
since its last slice". A system with a period does not run every slice, and the first
version handed it the tick's events when it ran: `record.lua`'s system, on 0.05 s,
never saw the `PlayerSeeked` it waited for. A system is now handed what came since it
last ran. The spec is corrected.

**Open questions closed:**

- **Scripts that read top to bottom** ship, as `kv.steps`: a record with one system that
  runs a function as a coroutine, with `s:wait(type)` and `s:wait(seconds)`. It is Lua
  over the record and adds nothing to the surface. The walk and the pass are written
  with it, and are a third the length they would be as handlers.
- **Ending the run** is `kv.quit(code)`, with `kv.args` for what followed the script.
  Both are `run`'s and not the spec's: a module does not end a run.
- **The user's script is the command line's equal**: `kv.has` says yes to every
  capability and a path is whatever the script says. The libraries are the spec's all
  the same (no `io` or `os`), so that what runs here runs as a module later.

**The walk** (BD20): 31 event types, 20 of them commands and each emitted by name; 10 of
the 11 notifications heard with every field their surface lists (an underrun cannot be
brought about on purpose, and is read by name all the same); 9 tables and resources,
each read and found to have every field; 79 fields and 39 enum values asked for by
name. The C++ side listens to every event type of both schemas with `engine_listen`,
writing no handler, and counts each command arriving natively; and checks the surfaces
list as many names as the schemas hold.

**The pass** (BD25, and the exit criterion): `pass.lua` adds a root, waits for its scan,
finds a track by a word, plays it from its row through the glue, hears it start and end
whole, hears the next start in another format with its `seq` after, seeks and hears the
seek, sets a band and hears it, finds the chain not flat and the output not exact,
pauses and finds the position held, and ends at the queue's end with no underrun.

**`blackwing run`** with no display: `raises.lua` exits 1 with
`raises.lua:11: attempt to index a nil value (local 'nothing')` and a traceback; a
script that returns 5 exits 5; the same with `--face dev`, the face up beside it.

**Tests** (`blackwing_script`, 40 checks; clean under `address,undefined`).

**Not built**, as the PRD has it: the manifest, versions checked, capabilities, quotas
and rate limits, the sandbox and the process boundary, a module's own tables and
events, stage and decoder states, discovery, `require`, snapshots. `on_change` notices
events and not tables.

## Milestone 6, the session

Nov 23, 2026. The same machine and build.

**As planned:** saved when it changes and at exit, restored paused through the API, the
player's new command, `ui_dev`'s part. Five open questions are closed here.

**The session is a module, `blackwing_session`**, not code in `blackwing`'s own. It is in
both engines (a library kept open with no face has a session too: the draft's choice
stands), and a face has to be able to name its two events, which `blackwing`'s module
cannot offer a face that does not depend on the executable.

**Its format is a file of its own**, `session.json`, written with `core`'s JSON, in the
settings directory KR24 gave the engine. It is not in the engine's settings store: a
queue is not a setting, and a queue in one file and a place in another would be two
moments. One file is one. The store is there for settings when Blackwing has some.

**A face hands over its part as events.** `SessionKeep{owner, key, number, text}`, and
`SessionKept` said back at the next start. The session knows no face and a face knows no
file. A table was the draft's other choice; an event is the same thing with nothing to
poll.

**`PlayerCue`** (BD32): a row and a frame taken up without playing. The transport says
`Paused` at that row and frame, nothing is heard, no device is opened, and the player
rests. A row has no rate or length until it has been heard, so the cue carries the ones
the session saved, and the transport shows those until the row plays; the test finds
the first frames heard after `PlayerPlay` to be the file's from that frame. A seek while
cued moves the place and stays cued.

**Found by restoring: a parameter set while nothing plays was never heard.** `hifi` says
a parameter is heard when a frame rendered with it reaches the ear. Stopped, cued or
paused, none does: the chain's table went on showing the old value, the player went on
asking for ticks to wait for it, and a session that set the volume at start-up saw the
default and would have saved that at the next quit. With nothing reaching the ear the
value is now the one that will be heard, and `PlayerParamChanged` says so in the next
tick. The face's readouts follow a slider while paused because of the same fix.

**How it knows something changed.** Tables do not say when they change (milestone 2's
finding), so four times a second the session hashes the rows' ids in order, the place,
the parameters and the kept values, and the frame when nothing plays. A row's id stands
for its path; the paths are read only when a file is written.

**What cannot be put back**, because the engine cannot say or set it (KR44):

- **Where the window was, and on which display.** `HostWindow` has a size and no
  position. The window comes back at its size.
- **A list's place.** A scroll box's offset is Krome's and has no setter. The library's
  list comes back at the right view, sort and search, at its top.
- **The window's size is read before the engine starts**, since the window opens in a
  module's start-up and cannot be resized after. `blackwing` reads the two numbers out
  of the session file itself, and works out where that file is with a copy of the
  engine's rule for Linux, because `platform_user_dir` needs an engine. On another OS
  the window would open at its own size until the engine can be asked earlier.

`ui_dev` keeps the window's size, the tab that is up, the library's sort and search,
and the way to where its list is. It has no splitters or column widths to keep.

### Measured

| what | |
|---|---|
| a thousand rows restored | 18 ticks, 64 rows a tick; the longest tick 0.8 to 1.3 ms |
| the file for a thousand rows | about 60 KB |
| a save while playing (a queue of two) | 3.7 KB, in place 4.3 ms after it was asked for |
| saves while playing | one every ten seconds (the position), and one 3 s after a change |
| saves while paused or stopped with nothing changing | none |

**In the window, by hand.** A queue of three (one of them a file that is not there), a
jump to the second, a seek to 95 s, the volume at -9 dB, the `rock` preset, the Events
tab, the Artists view, a window of 1000 x 700; quit at 99.6 s. Started again with no
size asked: a window of 1000 x 700, paused on row 2 at 99.6 s of 240, the volume and
the preset's bands as they were, the Events tab up, Artists in the library; `player
play` and it is at 101.4 s two seconds on.

**Killed, by hand** (`kill -9` after 27.0 s of playing, the volume at -4 dB): the file
left has the place at row 1 at 23.3 s. Started again: paused at 23.3 s of the same
track, the volume at -4 dB, both rows queued. 3.7 s were lost, of a bound of ten.

**Tests** (`blackwing_session`, 47 checks; `blackwing_player`'s cue, 16; `ui_dev`'s
part, 17; clean under `address,undefined`).

