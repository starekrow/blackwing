# Blackwing Slice 4 — The development face

Nov 23, 2026 · draft, first review folded in · built Nov 23 and 24, the close in part: see [As built](#as-built)

## Goal and exit criteria

Slices 1 to 3 built a player and a library with no face: everything is commands in and
tables out, and so far the only customers have been tests and a command line written by
the same hand as the API. Slice 4 is the first customer that is not. `ui_dev` is a
window over the API and nothing else, and a Lua script is the same thing with no window.
The [master PRD](Blackwing-Master-PRD.md) gives the bar: everything in slices 1 to 3 can
be driven and watched from the face, and from a script with no face at all.

Four things are built: `ui_dev`, the plain, dense, complete face that the rest of
Blackwing will be developed through; a window in `blackwing`, with a loop that keeps the
player ticking whatever the window is doing; the session, so that what was queued,
where it had got to and how the window was left are there at the next start (B28); and
the first piece of the script host, the bus surface in Lua, for a script the user runs.

The risk is different again. Nothing here is new audio or new storage. What can go wrong
is that the API turns out to be missing what a real customer needs, that a window in the
process costs the audio path something, that a face which looks idle is not, and that
fifty thousand rows which answer in 20 ms still do not scroll. So the tests are the face
driven headless against the capture device, the slice 1 disk runs with the window as the
load, and counters that say how many frames, layouts and wakeups a still window cost.

It is done when:

- **Everything can be driven from the face.** With no command line after the window is
  up: add a root, browse it by folder, artist, album and genre, search it, play a track,
  queue a folder, reorder and empty the queue, pause, seek by pressing and by dragging,
  skip, change the volume, move an EQ band, pick a preset, correct a title, remove the
  root. A test does all of it through the face's own controls, against the capture
  device.
- **Everything can be watched from it.** The transport in the device's time; every queue
  row with its state and reason; the device, its format, shared or not, exact or not,
  and why not; the lead, the last recut and jump; the chain and which stages are flat;
  the roots, the scan and the slot; and every event as it happens, with its `seq`.
- **Anything published is visible without a panel written for it.** A table or a
  resource of the player or the library, named, shows its rows and fields from its own
  description. A field added next slice appears with no change to the face.
- **The face is a customer and nothing more.** `ui_dev` includes the player's and the
  library's declarations and calls no function of either. Its tests run with neither
  module in the engine: a stand-in publishes the tables and reads the commands.
- **A script does the same with no face.** `blackwing run script.lua`, headless: a
  script adds a root, finds a track, plays it, seeks, sets a band, reads the transport
  and exits with a code. Every command, notification, table and resource of the two
  APIs is reached by the name native code uses, and a test walks them all.
- **Fifty thousand rows scroll.** Dragging the scrollbar through all tracks, end to end:
  no frame longer than one refresh of a 60 Hz display, rows not yet answered drawn as
  placeholders, and the rows in view filled within 100 ms of the scroll coming to rest.
- **A control answers in the frame it is touched.** A dragged slider follows the
  pointer; the command leaves in that frame; what is heard follows by slice 2's numbers.
  The readout beside it shows the value heard, not the value asked for.
- **Still is free.** Paused, stopped, or playing with nothing on screen changing: no
  frame drawn and no layout run, counted over ten seconds. Playing with the position
  showing costs what the position costs and no more; the number is in the notes.
- **Hidden is asleep.** Minimized, covered or on another workspace: no frame, no layout,
  no query refilled, and the music plays on with no underrun.
- **The window never costs a glitch.** Slice 1's ninety-second runs on all three disks,
  with the face up, the fifty thousand being scrolled and the window being resized the
  whole time: no underrun. A main thread held for two seconds (a resize the compositor
  stalls, a dragged title bar) is not heard.
- **A broken face is not a broken player.** A screen that does not load, or a face
  script that raises, shows its error where the screen would be, and the music goes on.
- **Every failure has a place on screen.** A track that fails or ends damaged, an
  offline root, a rebuilt index, an underrun, a path too long to play: each shows where
  the thing itself shows, with its reason in words.
- **It comes back as it was left.** Quit in the middle of a track with a thousand rows
  queued, and start again: the same queue in the same order, the place at the same row,
  the position within a second of where it was, paused; the volume, the mute and every
  band as they were; the window where it was, at its size, on the same panel, its list
  at the same place. The window answers before the queue has finished coming back.
- **Killed, it loses seconds.** After `kill -9` while playing, the next start is within
  ten seconds of where the position was, and nothing else is lost.
- **A session that cannot be read is set aside**, with a message, and the start is an
  empty one. A queued file that has gone is still a row, and fails when it is reached.
- **It still runs with no display.** `play`, `library` and `run` work on a machine with
  no display at all, and every test of this slice is headless.
- Numbers, in the notes: a still window's wakeups a second; frames and CPU while
  playing, shown and hidden; the scroll's worst frame and time to filled; start to a
  window that answers, and to a thousand rows restored; what the session writes and how
  often; memory held with the face up.

## What the code has

- **The player's API** (`korvid/blackwing_player/player.h`): eleven commands, seven
  notifications, `player.queue`, `player.chain`, `player.transport`, `player.output`.
- **The library's API** (`korvid/blackwing_library/library.h`): six commands, three
  notifications, `library.roots`, `library.rows`, `library.scan`. A window is at most
  512 rows, and 4,096 across every open window.
- **`blackwing`**: headless, with a loop of its own that ticks at the clock's rate while
  something plays or scans, and four times a second otherwise. The glue that makes a
  played file the library's priority is here. So are things only a command line needs:
  `library find`'s numbered list, and `player eq preset`, which reaches into `hifi_eq`
  for the presets.
- **Nothing is kept between runs** but the library's own: its index, its list of roots
  and the user's overrides, under the engine's per-user directory
  (`platform_pref_path`). The engine has that one directory, a rename that replaces a
  file whole (`io_rename`), and `save`, which writes any struct with a field table as
  an object of its fields by name and reads it back, tolerant of fields added and
  removed. It has no store of settings and names no directory for them.
- **`ui_dev`**: a page. Its `module.lua` names `hifi` as a dependency, which a face has
  no business with.
- **The script-module spec** ([docs/specs/script-modules.md](../specs/script-modules.md)),
  a draft with nothing built: `ctx:emit`, `ctx:resource`, `ctx:ktable`, `ctx:received`,
  the record a module returns, and a `SCRIPT.md` beside each module with a surface.
- **Krome** (Korvid): screens in markup or Lua over a model; a standard prelude with
  tables with sortable and resizable columns, trees, tabs, splitters, sliders, dropdowns,
  menus, dialogs and edits; a virtualized `each` of fixed-height rows over a list the
  model holds whole; focus order and shortcuts; a model set from C++ (`krome_set`,
  `krome_set_doc`) and events out (`KromeEvent`); a Lua state of its own that only
  events cross; a still screen that costs no layout and no paint; a view with no window
  (`krome_viewport`, `krome_press`, `krome_settled`, `krome_stats`) for tests; reload of
  a screen while it runs. No shaping beyond kerning, and left to right only.
- **`frameloop`** (Korvid): the window, the renderer and the loop, as goldberg's window
  uses them. Paced on demand it draws only when something happened, and it runs ticks
  only then too: a tick and a frame come together.
- **`script`** (Korvid): a ktable in Lua, by its field table. Nothing for events, and a
  cosmetic resource has no field table (KR17 and KR18, written up and not yet put).
- **Nothing in the platform's headers says a window is hidden.**

## The shape

### Where things are

| module | this slice |
|---|---|
| `ui_dev` | the face: its screens, and the C++ that carries tables to them and their events to commands |
| `blackwing` | the window and its loop; which face is up, or none; the session; `run` |
| `blackwing_script` | new: the host. This slice, the bus surface in process, for a script the user runs |
| `blackwing_player` | what the face and the session find missing: parameters that describe themselves, presets as data, a path of any length, a place taken up without playing |
| `blackwing_library` | what the face finds missing: tracks by their paths, and whatever queueing a whole row needs |

`ui_dev` depends on `core`, `krome`, `blackwing_player` and `blackwing_library`, the last
two for their headers. `hifi` leaves its list.

### One loop, two rates

The player needs its ticks while it plays: they feed the pieces and publish the
position. The window needs a frame only when something on it changed. These are two
rates, and audio's is the one that is never given up.

- **The player ticks whatever the window does**: hidden, still, being resized or mid
  drag.
- **A frame is drawn when a screen changed**, and not because a tick ran.
- **Nothing playing and nothing changing** is the headless loop's four wakeups a second
  or fewer.
- **The loop is `blackwing`'s to own.** Whether it is `frameloop` with a new pacing or
  the headless loop grown a window is milestone 1's question, and KR20's.

### The face is a reader and an emitter

`ui_dev`'s C++ does two things: it copies what the tables say into each screen's model
when it changes, and it turns a screen's events into commands. It holds no state of the
player's or the library's that it could not read again. What it holds of its own is
where the user is: which panel, which view, how far scrolled, what is selected.

- **A model is written when its source changed**, not every tick. The position is the
  one thing that changes all the time, and it is written at the rate it can be seen to
  change: the readout's digit, the bar's pixel.
- **A window onto the library is open only while its list is on screen.** A panel that
  is not showing closes its windows.
- **What is in view is what is prioritized.** When a list of tracks comes to rest, the
  face emits `LibraryPrioritize` for its window. This is the master PRD's "looking at
  something scans it next", and the first time anything looks.
- **A dragged control sends its value each frame**, and the player renders the newest
  (slice 2). The control shows where the pointer is; the number shows what is heard.
- **The screens are markup and Lua, read at start**, so a panel is changed without a
  build and reloads while the music plays.

### What it shows

Dense and plain: Krome's standard prelude in its dark theme, no design of its own. One
window, panels beside each other and in tabs.

| panel | watch | drive |
|---|---|---|
| Transport | state, the track and its title, position and length in the device's time, a seek on its way | play, pause, stop, next, previous, seek by press and by drag, seek by seconds |
| Queue | every row: place, title, state, format, bytes held, reason | jump, remove, move by dragging, clear, add a path typed in |
| Library | roots with state and counts; the views; search; rows with what is missing, offline or unreadable | add and remove a root; open a folder, artist, album, genre; sort; queue or play a row of any kind; edit a field |
| Output | device, format, shared, exact and why not, the lead in seconds, underruns, the last recut and jump | |
| Chain | each stage, flat or not, every parameter with its value heard | volume and mute; every band; a preset; any parameter by its own description |
| Scan | what is being walked and read, the rates, the slot, unread counts | |
| Events | every notification and command as it passes, with `seq`, newest last; held, filtered by type | |
| Tables | any table or resource, by name: rows and fields from its description | |

**Exact, and why not.** The indicator is `player.output`'s `exact`. The reason is
worked out by the face from what is published: the device is shared, a stage is not
flat (which), a join is under way, the device's word is narrower than the file's. If
the face cannot say why from the tables alone, the tables are missing something, and
that is a finding.

### What the API is missing

Found by reading the headers as a face would. Each is a change to the player or the
library, small, and a face other than this one needs it too.

- **A queue row has a path and no title.** The face must ask the library what a path is.
  The library answers for a list of paths, as a window like any other. The player still
  knows nothing of tags.
- **A parameter has a name and a value, and no range.** A slider cannot be drawn for a
  stage the face has never heard of. A stage's parameters say their least, most,
  default, step and unit.
- **Presets are reached by calling `hifi_eq`.** They become data the player publishes
  beside the chain, and applying one is a command.
- **Queueing an album is a loop in the caller.** A window is 512 rows at most, and a
  folder with its subfolders has no window at all. Queueing a row of any kind, in its
  own order, is one command. The player and the library stay strangers, so this lives
  where the other glue does, in `blackwing`, or the library hands back a list the glue
  forwards.
- **A place cannot be taken up without playing.** `PlayerSeek` on another row is a jump
  there, and it plays. The session needs "at this row, at this frame, paused", and so
  will a face that lets a track be cued.
- **A path longer than 511 bytes cannot be queued** (slice 3's Open). It is settled
  here, before a second face is written against the event: a track at any path the OS
  allows is queued from its row.

The build will find more. Each one found is fixed in the API, not worked round in the
face, and listed in the notes.

### The session

The session is `blackwing`'s: the application remembering how it was left. The player
stays a player that another project could link, with no files of its own, so the
session is one more customer. It reads the tables to save and emits commands to
restore, and what it cannot do that way is a gap in the API.

- **What is kept**: the queue's paths in order; the place and the frame heard; every
  parameter of the chain, the volume and the mute among them; and, for each face, a
  part of its own. `ui_dev`'s is the window's size and position, the panel showing,
  the splitters and column widths, and the library view with its sort, search and
  scroll. The roots are the library's and already kept.
- **It comes back paused.** A program that makes sound by being started is a surprise;
  one press plays from where it was.
- **It is saved when it changes**, a few seconds after the last change so that a drag
  is one write, and at exit. While playing, the position is saved every ten seconds.
  Paused or stopped with nothing changing, nothing is written: still is free here too.
- **A write is whole or not at all**: a file written beside the old one and renamed
  over it, below the player's priority in the I/O queue, never on the audio path.
- **It is restored through the API**, after the window is up: the rows are enqueued,
  the place taken up, the parameters set. A thousand rows must not hold the first
  frame, and ten thousand must not hold a tick.
- **It has a version**, and reads an older one. A field it does not know is ignored and
  one it lacks keeps its default, as `save` does.
- **One that will not parse is set aside**, not overwritten, with a message; the start
  is empty.
- **A row whose file has gone is restored anyway.** The disk may be unplugged today and
  back tomorrow. It fails with a reason when it is reached, as any row does.
- **A window is put back where it can be seen.** A position on a monitor that is no
  longer there is not used.
- **`play FILE...` and `run` neither restore nor save.** They are a command, not a
  sitting. The face, and `library` kept open, do both. `--no-session` turns it off.
- **It lives where the OS keeps settings**, which the engine does not name yet (KR24).
  Until it does, it is a file in the per-user directory beside the library's.

### The fifty thousand, on screen

Slice 3 made a window of rows answer in 20 ms. A scrollbar dragged across fifty
thousand rows passes thousands a frame, so the list cannot wait for its rows.

- **The list knows its length and draws at once.** A row not yet answered is a
  placeholder of the right height.
- **A query is sent for where the scroll is, not for everywhere it has been**: the
  newest replaces the one before, which the library already stops.
- **The window asked for is wider than the view**, so a wheel's notch is already there.
- **Rows that change under an open list** (a scan filling in tags) change in place,
  without the list jumping.

Krome's virtualized list wants every row in the model. Fifty thousand rows of five long
strings is not something to hold twice, so this asks Krome for a list over a window
(KR22).

### Text

A library's titles are in every script there is. Krome draws left to right with no
shaping, from whatever fonts are loaded.

- **No title breaks a row.** Text the face cannot draw properly is still one line of
  the right height, clipped, with nothing thrown and nothing overlapping.
- **What is wrong is written down.** The close runs a list of real titles (Chinese,
  Japanese, Korean, Arabic, Hebrew, Devanagari, Thai, combining marks, emoji) and
  records what each looks like. That list is what the conversation with Krome about
  text starts from, before slice 6 needs it answered.

### Scripts

The spec's host is P1: modules found on disk, a manifest, capabilities, a sandbox. What
this slice builds is the part of it that B2 has asked for since the first draft, and it
is built as the spec has it so that P1 moves it and does not rewrite it.

- **`blackwing run FILE.lua`** loads one script, in process, headless or with a face up.
- **The script returns the spec's record**: handlers and systems, with their `reads`
  and `emits`. `ctx` is the spec's: `emit`, `resource`, `ktable`, `received`, `now`.
- **The binding is made from the field tables** the modules already carry, with the
  spec's mapping of types and its errors. Nothing is typed in by hand per event.
- **`kv.surface`, `kv.enum`, `kv.has`, `kv.log`** as the spec has them, so a script can
  ask what there is.
- **It is the user's script, run by the user's command.** It is the command line's
  equal: it may name a path, and nothing is granted or refused. The spec's rule that a
  script names no path is the host's to enforce for modules, in P1.
- **A script that raises says so, with its file and line, and is stopped.** The player
  is not.
- **A script can end the run**, with a code, so a test is a script.

Not built: the manifest, versions checked, capabilities, quotas and rate limits, the
sandbox and the process boundary, a module's own tables and events, stage and decoder
states, discovery. The `SCRIPT.md` pages for `player` and `library` stop being drafts
where this slice proves them, and are corrected where it proves them wrong.

### When things go wrong

- **A screen that will not load** leaves its panel showing the file, the line and the
  message. The other panels work.
- **The face's Lua raising** in a handler loses that one event.
- **No display, or no renderer**: `blackwing` says so in a line and runs headless if it
  was given something to do, and otherwise exits with that as its error.
- **The window closed** ends the run, as interrupting it does: the session is saved and
  the device is closed cleanly. A player that goes on playing with no window is slice 5's, with the system's
  now-playing surface to bring it back.

### How it is tested

- **Headless, through the face's own controls.** Krome's view with no window, presses
  by node id, the capture device underneath. The test for "driven from the face" emits
  no command itself.
- **Against a stand-in.** A test module that publishes the player's and the library's
  tables and records the commands it is sent. The face's panels are tested on tables
  made by hand: a failed row, an offline root, fifty thousand rows that answer late.
- **By counters.** Krome's layouts and paints, the loop's frames and wakeups, read
  before and after ten still seconds.
- **By script.** The conformance walk, and a scripted pass over what slices 1 to 3
  promised that a script can check.
- **By starting twice.** The session's tests run `blackwing` against a data directory
  of their own, stop it (cleanly, and with a kill), and start it again.
- **On the three disks**, with a real window, by hand and by `stress-play.sh`.

## What the engine gets

Written up as KR20 to KR25 in
[Korvid-Changes-For-Blackwing.md](Korvid-Changes-For-Blackwing.md), with KR17 and KR18
raised from P1.

- **KR17, a cosmetic resource's fields**, was P1 for script modules. The Tables panel, a
  script's `ctx:resource` and the session all need it now. Blocks milestones 4 and 5;
  without it the face and the host each carry a copy of three layouts.
- **KR18, events in Lua**, the same. Blocks milestone 5; the workaround is the ktable
  binding's code written again in the host.
- **KR20, ticks without frames.** A window's loop in which a module can ask for ticks
  at the clock's rate while no frame is drawn, and a frame is drawn only when a screen
  changed. Blocks milestone 1 unless `blackwing` can keep its own loop and ask
  `frameloop` only to render; which it is wants a conversation before it is built.
- **KR21, a window says when it is hidden and shown**: minimized, covered, on another
  workspace, the screen off. As good as each OS can tell. B25 cannot be met without it.
- **KR22, a virtualized list over a window of rows.** The list is told its length, says
  which rows are in view, and draws a placeholder for a row the model does not hold.
  Blocks milestone 3's scroll; the workaround is the face doing a scroll box's
  arithmetic with spacers of its own.
- **KR23, the event trace as data**, so the Events panel sees every type without a
  handler written for each. Wanted; the handlers are the workaround.
- **KR24, preferences storage on every platform**: where each OS keeps a program's
  settings, apart from its data and its cache, and a small store of named values kept
  there, written whole. Wanted by milestone 6; the workaround is a file of our own in
  the per-user directory, which is right on Linux and nowhere else.
- **KR25, a JPEG decoder in `image`**, a decent one: baseline and progressive, the
  orientation honoured, decoded at a fraction of its size for a thumbnail, safe on a
  hostile file. Nothing in this slice shows art; it is asked now so that it is there
  when BL25 and slice 6 want it.

Not asked for, and noticed: `frameloop` brings sprites, scenes and a touchpad into a
music player's link; and a face's Lua cannot read a table, so every screen has C++
beside it. Neither is in the way yet. Text is a conversation, above.

KR19 (registering after start-up) is not needed: this slice's script declares nothing
of its own.

## Out of scope

- **Choosing a device, hot-plug, media keys, keep-awake, Windows and macOS.** Slice 5.
- **The visualizer and `hifi.meter`**, and the other three faces. Slice 6.
- **Any design.** `ui_dev` is the standard prelude. What a good player looks like is
  slice 6's question.
- **Art.** Nothing records it yet (BL25, owed), so nothing shows it.
- **Changing face while it runs**, a second window, a settings file.
- **Files dropped on the window, file associations, a single instance.** B41, P1.
- **Script modules**: everything the spec has past the bus surface. B33, P1.
- **An accessibility tree, right-to-left text, shaping.** The engine's, and not yet.
- **Playlists.**
- Still owed and not this slice's: BR22, BL23, BL25, KR15, KR16.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| BD1 | `blackwing` opens a window with a face, or runs with none, chosen on the command line; headless needs no display | P0 |
| BD2 | The player ticks at its rate whatever the window is doing; a frame is drawn only when a screen changed | P0 |
| BD3 | A still window draws nothing and lays out nothing; a hidden one runs nothing of the face | P0 |
| BD4 | `ui_dev` reads tables and emits commands, and calls nothing of the player or the library | P0 |
| BD5 | Transport and Queue panels: every transport and queue command, every field of the transport and the rows | P0 |
| BD6 | Output and Chain panels: the device, the lead, exact and why not; every parameter; presets | P0 |
| BD7 | Library panel: roots, every view, search, sort, a row of any kind queued or played, a field edited | P0 |
| BD8 | A list of fifty thousand rows scrolls with placeholders, the newest query winning | P0 |
| BD9 | What is in view is prioritized; a window is open only while it shows | P0 |
| BD10 | Scan and Events panels | P0 |
| BD11 | The Tables panel: any table or resource by name, from its description | P0 |
| BD12 | A queue row's title: the library answers for a list of paths | P0 |
| BD13 | A stage's parameters describe themselves: range, default, step, unit | P0 |
| BD14 | Presets are published as data, and applying one is a command | P0 |
| BD15 | Queueing a folder, album, artist, genre or search is one command | P0 |
| BD16 | A track at any path the OS allows is queued from its row | P0 |
| BD17 | Every failure in the master PRD's list that slices 1 to 3 can produce shows in the face with its reason | P0 |
| BD18 | A screen or face script that fails shows its error and stops nothing else | P0 |
| BD19 | `blackwing run`: the bus surface in Lua as the spec has it, made from the field tables | P0 |
| BD20 | The conformance walk: every name of the player's and the library's API reached from a script | P0 |
| BD21 | The face's tests: headless through its controls, and against a stand-in | P0 |
| BD22 | The disk runs with the window as the load, and the two-second stall | P0 |
| BD23 | Every control reached and worked from the keyboard; the transport's shortcuts | P1 |
| BD24 | The queue reordered by dragging | P1 |
| BD25 | The scripted pass over slices 1 to 3's criteria | P1 |
| BD26 | The list of titles, and what each looks like | P1 |
| BD27 | The runs owed from slices 2 and 3: the hour, and ninety seconds under a scan, with the face up | P1 |
| BD29 | The session: the queue, the place and frame, the chain's parameters, and each face's own part, saved when changed and at exit, restored paused through the API | P0 |
| BD30 | A session's write is whole or not at all, below the player's priority; nothing is written while nothing changes | P0 |
| BD31 | A session that will not parse is set aside; a version is read forward; a missing file is still a row | P0 |
| BD32 | The player takes up a row and a frame without playing | P0 |
| BD33 | The position while playing is saved every ten seconds, so a kill loses no more | P0 |
| BD28 | Module pages for `ui_dev` and `blackwing_script`; the `SCRIPT.md` pages corrected; numbers in `docs/design/slice-4-notes.md` | P1 |

## Milestones

Each leaves the tree building and its tests passing.

1. **The window** (BD1 to BD3, BD18, and BD22's stall). A window, a loop with two
   rates, one screen with play, pause and the position: enough to prove a table reaches
   the screen and a press reaches the player. Still, hidden and stalled are measured
   here, before there is anything to hide them behind. Needs KR20 and KR21 answered.
2. **The player's panels** (BD4 to BD6, BD13, BD14, BD21). Transport, Queue, Output,
   Chain, against the stand-in and then the player. The API's changes for parameters
   and presets.
3. **The library's panels** (BD7 to BD9, BD12, BD15, BD16). Roots, the views, search,
   the scroll, titles in the queue, queueing a row, the long path. Needs KR22 or its
   workaround.
4. **The inspectors** (BD10, BD11, BD17). Scan, Events, Tables, and every failure given
   its place. Needs KR17 for resources.
5. **Scripts** (BD19, BD20, BD25). The host's first piece, `run`, the walk. Needs KR17
   and KR18. It touches nothing of milestones 1 to 4 and can be built beside them.
6. **The session** (BD29 to BD33). Saved and restored through the API, the player's
   new command, `ui_dev`'s part. It is after the panels so that there is something of
   the face's to keep, and it uses KR24 if it has come and a file of its own if not.
7. **Close** (BD22 to BD28, what is left of them). The disk runs, the owed runs, the
   keyboard, the titles, the numbers, the pages, the master PRD corrected where this
   slice proved it wrong.

The window is first because the loop is the one thing here that can hurt the audio
path, and it is cheapest to measure with one screen. The player's panels come before
the library's because they are small and find the pattern: a table to a model, an event
to a command. Scripts are last in the list and first in independence.

## Decided

Nov 23, 2026, in review of the first draft.

- **The session is in this slice** (B28), and with it the engine is asked for
  preferences storage on every platform (KR24).
- **Closing the window ends the run.**
- **Korvid is asked for a decent JPEG decoder** (KR25), ahead of the art that needs it.
- **The engine asks are written up**, KR20 to KR25.

The draft's other choices, not yet argued with:

- **`ui_dev` is Krome's standard prelude and no design.**
- **The face's screens are markup and Lua with C++ carrying the data**, not all C++ and
  not all Lua.
- **Slice 4 builds the script host's first piece**, to the spec, in process and
  trusted, rather than a throwaway binding for B2.
- **A script the user runs is the command line's equal**: paths allowed, nothing
  granted.
- **What the face finds missing is fixed in the API**, in this slice.
- **The long path is settled here.**
- **The session is `blackwing`'s, comes back paused, and is restored through the API.**

## Open

- **The loop**: `frameloop` with a pacing it does not have, or `blackwing`'s own with a
  renderer. It decides what KR20 asks for.
- **Where queueing a whole row lives**: glue in `blackwing` that a script would have to
  repeat, or a command of the library's that answers with paths for the glue to
  forward. Neither may teach the player about the library.
- **How the long path is carried**: a longer field, or a table of paths and an id. It
  touches `PlayerEnqueue`, `PlayerItem`, `LibraryRow`, `LibraryAddRoot` and
  `LibraryPrioritize`, and the script surface's version.
- **The session's format**: `save`'s JSON by field tables, or whatever KR24's store
  turns out to hold. And whether a queue of ten thousand rows belongs in a settings
  store at all, or beside it as a file.
- **Whether the session is saved headless** (`library` kept open with no face), as the
  draft has it, or only with a face up.
- **How a face hands over its part**: a table it publishes for `blackwing` to keep, or
  a call. A table keeps `blackwing` from knowing any face.
- **Scripts that read top to bottom.** The spec's record is handlers. A test wants
  "play, wait for started, seek, wait for seeked". A helper in Lua over the record (a
  coroutine that waits for an event) adds nothing to the surface; whether it ships, and
  as what, is open.
- **How often the position is written.** By the digit and the pixel, as above, or a
  fixed few times a second.
- **Whether a face's Lua should read tables itself.** With KR17 and KR18 the binding
  exists; Krome's Lua state is its own by decision. `ui_dev` will show how much C++ a
  panel costs without it.
- **The numbers**: one refresh as the scroll's worst frame in a dev build; 100 ms to
  filled; two seconds of stall; ten seconds of stillness; how much wider than the view
  a window is asked for; ten seconds between saves of the position, and how long after
  a change the rest is saved.
- **Whether the Events panel shows commands too**, or only what the two modules emit.
  Commands are what a face author most wants to see.
- **Requirement IDs.** `BD`, after `BS`, `BR` and `BL`.

## As built

Nov 24, 2026. Milestones 1 to 6 are built and tested; the close is in part. The findings
and every number are in [the notes](../design/slice-4-notes.md), with the exit criteria
one by one at their end.

**The open questions, as they were closed:**

- **The loop**: Korvid's frame loop paced `Decoupled`, with a face; the loop `blackwing`
  had, with none, over an engine that holds no window module. Both tick while a module
  asks (`engine_want_ticks`).
- **Where queueing a whole row lives**: a command of the library's (`LibraryCollect`)
  that answers with paths, forwarded to the player by `blackwing_glue`, which is a
  module now.
- **How the long path is carried**: a longer field, 4096 bytes, in every event and row
  that holds a path.
- **The session's format**: a file of its own, JSON, in the settings directory; not the
  engine's settings store.
- **Whether the session is saved headless**: yes, for `library` kept open.
- **How a face hands over its part**: events, `SessionKeep` and `SessionKept`.
- **Scripts that read top to bottom**: `kv.steps`, shipped as part of `run`.
- **How often the position is written**: by the digit and the pixel. The pixel is dear:
  eight a second on a two-minute track, each drawn twice.
- **Whether a face's Lua should read tables itself**: not asked. `ui_dev` is about 1,900
  lines of C++ for eight panels, most of it formatting; the cost was not the carrying.
- **Whether the Events panel shows commands**: it does.
- **The numbers**: as measured, in the notes.

**New modules**: `ui_dev` (the face), `blackwing_window`, `blackwing_glue`,
`blackwing_session`, `blackwing_script`.

**What the API gained**, each because a customer needed it: field tables for the three
resources; `player.params` and `player.presets`, `PlayerApplyPreset`, a parameter's
step and unit; `player.output`'s `bits`; `PlayerCue`; a parameter heard at once when
nothing sounds; `LibraryPath` and the `Paths` view; `LibraryCollect` and
`LibraryCollected`; paths of any length; `library.scan`'s `slot` made true.

**What the engine is asked** beyond KR17 to KR25: KR26 (a still window polls), KR27 (the
check and `placeholder`), KR43 (a text read back whole), KR44 (a window's place, a
list's, a directory before the engine), KR45 (a present that blocks holds the ticks).

## Owed, as of the close

- BD22: the three disks with the window as the load; a real device; a resize through a
  run.
- BD24: the queue reordered by dragging.
- BD27: the hour; and ninety seconds under a scan with the window seen throughout.
- A ship build's numbers.
- The window's place and a list's (KR44); an override file set aside shown in a face
  (the library does not publish it); an artist's tracks with no album in a view.
- Still owed from before: BR22, BL23, BL25, KR15, KR16.

