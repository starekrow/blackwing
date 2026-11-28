# ui_dev

The development face: a plain, dense, complete window over the player and the library.
It is built first, and it is how the rest is developed. It reads their tables and emits
their commands, and calls no function of either.

**Status:** built in slice 4. Every panel is in: Transport, Library, Queue and Output,
and in tabs under Output, Chain, Scan, Events, Tables and (slice 5) Media; its part of the session; the
keyboard. Owed: the queue reordered by dragging, and the
[slice's](../../docs/PRDs/Blackwing-Slice-4-PRD.md) runs on real disks and a real
device.

**The keyboard.** Space plays or pauses, from anywhere but an edit; Ctrl with Left and
Right seeks ten seconds, with Up and Down goes a row back and on, with S stops. Every
control is reached by Tab and worked with Space or Enter.

## How it works

**One screen, of panels.** `assets/shell.kim` is the face's one Krome screen, in
`krome/std`'s dark theme. Each panel is a definition (`queue-panel`) in a file of its
own, `assets/<name>.kim`, with its model's first values in `assets/<name>.lua` beside it;
the shell `use`s the one and names the other as a `script`. `assets/widgets.kim` and
`widgets.lua` are the few styles and the one behaviour the panels share. Krome's screens
are whole-window layers, so panels cannot be screens side by side.

**A reader and an emitter.** The C++ does two things, per panel, in two units:

- `ui_dev.sync` (PostSim) reads the tables through the context and writes the panel's
  part of the model. The writer (`src/model.cc`) keeps a hash of what each key last
  showed, and a write of the same value is nothing. One that goes through asks the loop
  for a frame, which is the only reason the face ever asks for one.
- `ui_dev.events` (a handler on `KromeEvent`) offers each of the screen's events to the
  panels by name, and the one that knows it emits the command. Where it needs a table
  to say which command (the row above this one), it reads it as the phase found it
  (`^player.queue`): what the screen was showing. The face then asks for four ticks
  (`engine_want_ticks`), since the command's answer is a tick or two away.

It keeps nothing of the player's or the library's that it could not read again. Its own
is where the user is: what a list has in view, which slider is held.

**The panels.**

| panel | file | shows | does |
|---|---|---|---|
| Transport | `transport.cc` | the state, the row and its file's name, position and length in the device's time, a seek on its way | play, pause, stop, next, previous, ten seconds back and on, and the bar: a press or a drag is a `PlayerSeek` each time it moves |
| Library | `library.cc` | the roots with their states and counts, and why one is offline; the list of whichever view is open, each row with what is missing, offline or unreadable | add and remove a root; folders, artists, albums, genres, every track, a search; sort; open a row, queue it, play it; edit a track's field |
| Queue | `queue.cc` | every row in the list's order: place, the library's title for it (else its file's name), state, format, bytes held, reason | play a row now, remove it, move it up or down, clear, add a path typed in |
| Output | `output.cc` | the device, its format, the lead, underruns, the last recut and jump; exact, and why not; what is chosen (the default followed, a device standing in, one lost, why none is open); every device in `player.devices` with its backend and whether it is playing, the default, chosen or gone | use a device (`PlayerSelectDevice` by its name), follow the default |
| Chain | `chain.cc` | each stage and whether it is flat; every parameter with the value heard | a slider a parameter, a switch for one that is 0 or 1, a button a preset |
| Scan | `scan.cc` | what is being walked and read, the rates, the slot, what is unread, the index and whether it was made again | |
| Events | `events.cc` | every command and notification of the two modules as it is delivered, with its fields and who emitted it, newest last | hold, clear, a filter |
| Tables | `tables.cc` | any table or resource of the two modules (and `media.status`), by name: its rows and fields from its own description | pick one |
| Media | `media.cc` | `media.status`: the name owned, what the OS is shown and its art, keep-awake and how, whether sleep is heard, the commands and reads from the OS; why there is no surface | |

**The position** is written every tick, as what would be shown: the readout's text
changes once a second and the bar's value once a pixel of its width, and the writer
does nothing in between.

**A control answers in the frame it is touched.** `dev-slider` is std's slider over 0 to
1 that emits `slide` as it moves and `slide_end` when let go (std's writes the model and
says nothing, and the C++ hears only events). While one is held its knob is the
pointer's: the face leaves that key alone. The readout beside it goes on showing the
value heard.

**The queue is a list over a window** (Krome's sparse `each`): it lays out as if every
row were there, says which it has built (`KromeView`), and `queue.cc` hands over those,
found in the table by `pos`. A queue of four thousand rows is sixty in the model.

**The library's list is one too, over a window of the library's.** The face asks for
where the list is (`LibraryQuery`, window `kFaceLibWindow`), half as wide again each side,
and again only when the list leaves what was asked, so a wheel's notch asks for nothing;
a newer ask replaces the one before, which the library stops. Rows not yet answered are
placeholders. Rows of another list still in the table are not shown as this one's
(`lib_stale`, until `LibraryResult` comes for what was asked). When a list of tracks has
been at rest for 0.3 s the face emits `LibraryPrioritize` for its window: what is looked
at is scanned next. A folder opened is prioritized at once, as a directory.

**A row of any kind is queued with one command**, `LibraryCollect`, by the row's kind and
id; the application's glue turns the library's answer into the player's commands. To
play a row the face empties the queue first, and emits `PlayerPlay` when the last
`LibraryCollected` of its id has come.

**A queue row's title is the library's.** The player knows paths. For each queue row in
view, and the transport's, the face sends `LibraryPath` (window `kFaceTitleWindow`, the
row's place as its index) once the view has been still for three ticks, and after that
only for an index whose path changed. A row is shown under the answer's title only when
the answer's file name is the row's, since the queue moves under the answers.

**A window onto the library is open only while it is looked at.** Hidden, the face closes
both; seen again, it asks again.

**Its part of the session** (`keep.cc`). The session keeps values for whoever asks
([blackwing_session](../blackwing_session/README.md)); this face is `ui_dev` and asks for
the window's size, the tab that is up, the library's sort and search, and the way to
where its list is, each when it is not what it last asked. At the next start it takes
them back from `SessionKept` before its screen is built. Where the window was is kept
too, where the platform lets a program place a window (KR42: not on Wayland, where the
place last kept is left as it was): it is put back once the window is up, and if it lands
with less than 64 pixels each way of it in its display's work area, or its top above it,
it is moved into the work area. How far a list was scrolled is not kept: the engine
cannot set it (KR44).

**The chain's controls are written from the tables.** `chain.cc` writes the definition
`chain-stages` as markup from `player.params` and `player.presets`, compiles it
(`kim_compile`) and gives it to Krome in the place of `assets/chain_stages.kim`, which
says only that there is none yet. It is written again when what it was made from
changes (a hash of the stages, the parameters' names and ranges, the presets). A stage
the face has never heard of gets its controls from its own description. It is not a
list because a list is built again when a row's value changes, which while a slider is
dragged is every tick, under the pointer.

**Events are the engine's trace, as data.** The face sets the engine's event hook
(`engine_set_event_hook`, dev builds) and keeps the last 512 events whose type's name
begins `Player`, `Library` or `Hifi`, each as the engine describes it. No type has a
handler, and one added later shows. They are kept only while the tab is up, the window
seen and the panel not held. In a ship build there is no hook, and the panel says so.

**Tables are shown from their descriptions.** `tables.cc` names no field: it reads the
field table the engine holds for a table's component or a resource, and formats each
value by its type. A field added to a table appears. A table added needs its name in
`k_tables` and in `ui_dev.sync`'s `reads`.

**Only the tab that is up works.** The tabs write the model's `r_tab` themselves; `.sync`
reads it, and Events and Tables return at once when they are not up.

**Exact, and why not** is worked out in `output.cc` from what is published: the device
is shared, a stage is not flat (which), a join is under way, the device's word is
narrower than the file's.

**Hidden, it is asleep.** On `WindowVisibility` saying the window is not seen, `.sync`
returns before it reads anything; seen again, the writer forgets what it showed and
everything is written in the next tick.

**A frame costs a tick.** `ui_dev.frame` (in the Frame cycle, so once for each frame
drawn) asks for one: what Krome said in a frame is an event the next tick delivers, and
a resting loop's next tick may be a quarter second away.

**A panel that does not load fails alone.** A mistake anywhere in a Krome screen's files
fails the screen. When the shell fails, `ui_dev` reads `krome_errors`: for each mistake
that names a panel's file it compiles a stand-in (a definition of the same name that
shows the file, the line and the message, in `widgets.kim`'s `panel-broken`), gives it to
Krome in the file's place with a script that does nothing, and opens the shell again. A
mistake that names no panel (the shell's own, the prelude's) leaves a screen that lists
the mistakes. Either way the player is not involved.

## Using it

Put the module in an engine with a window (or a Krome viewport) and something that
publishes the player's tables; the face is up. `blackwing` does; so do this module's
tests, with a stand-in.

| name | what |
|---|---|
| `ui_dev_configure(UiDevConfig)` | before `engine_init`: `enabled` (false: in the engine and doing nothing), `open` (false: opened by `ui_dev_open`) |
| `ui_dev_open(e)` | opens the screen, or opens it again from the files as they are |
| `ui_dev_screen(e)`, `ui_dev_ready(e)` | the screen's name in Krome; whether it is built and written |
| `ui_dev_stats(e)` | model writes, commands, ticks looked at and passed over, panels standing in, whether the screen failed |

On the engine's command line: `face press ID` presses a control by its id, `face wheel
ID NOTCHES` turns the wheel over one, `face show KEY` prints a model value as the screen
shows it, `face stats` prints what the face and Krome have done. A real window can be driven and read by a script through them.

What it registers: `ui_dev.sync` (PostSim; reads the player's two resources and four
tables and the library's resource and two tables; emits `LibraryQuery`,
`LibraryPrioritize`, `LibraryPath`, `PlayerPlay`), `ui_dev.events` (on `KromeEvent`;
emits the player's twelve commands and four of the library's), `ui_dev.view` (on
`KromeView`), `ui_dev.result` and `ui_dev.collected` (on the library's answers), `ui_dev.kept` (on
`SessionKept`),
`ui_dev.visibility` (on `WindowVisibility`; emits `LibraryClose`), `ui_dev.frame`
(Frame).

## Tests

`tests/` runs the face with no window: Krome's view at a fixed size (`krome_viewport`),
controls pressed by id (`krome_press`) or by a pointer and a keyboard the test moves
(`input_pointer_override`, `input_text_override`).

- **Against stand-ins** (`standin.cc`): a module named `player` and one named `library`
  that register the two APIs' events, tables and resources from their `schema.h`,
  publish values the test sets by hand and write down the commands they are sent.
  Neither the player nor the library is in the engine. The library's panel: every view
  opened from a row, queue and play, an edit, sort, search, roots, titles, and fifty
  thousand rows that answer two frames late while the scrollbar is dragged end to end.
  The inspectors: Scan from the resource; Events found by their described text, filtered,
  held and cleared; Tables with every field of the schema found by its name. Every transport command from its own control;
  the bar pressed, dragged and let go; a thousand queued rows scrolled, edited and added
  to; exact and each reason why not; the chain's controls made from descriptions, a
  slider held while the value heard changes under it, a parameter added while it runs;
  ten still seconds costing no frame, layout, paint, model write or request; hidden,
  nothing read or written; a panel whose file is not markup saying so in its place.
- **On the player and the library**: the same face over `blackwing_player` on a capture
  device on the wall clock, `blackwing_library` on a directory the test makes, and
  `blackwing_glue`. A path is typed into the edit and entered, played, the volume
  dragged, a preset pressed, the bar pressed, paused, played to its end, the row removed;
  then a root typed in, browsed by every view, searched, the one found played, its title
  corrected, an album queued from its row, the root removed. The test emits no command
  and reads only what the screen shows.

## Behaviour worth knowing

- **A stand-in for a broken panel stays until the next start.** What Krome is given by
  hand (`krome_provide`) wins over the file, so mending the file needs a restart.
- **The model is one table for every panel**, since they are one screen: keys carry
  their panel's letter (`t_state`, `q_rows`, `o_why`, `c_v_1_14`).
- **A row's buttons carry its id** (`qx_17`, `cs_1_14`, `lq_3`), and the C++ reads the
  number back out of the node's name: an event carries a node and one number, no more.
- **A cell is a box that clips**, with its text inside (`dev-text`): a text on one line
  is as wide as its words, whatever room its row gives it.
- **A sparse list's `placeholder` block** is the `each`'s own (`count=` or `from=`), and
  Krome's check knows it (KR27, Korvid slice 12): a `placeholder` anywhere else is refused
  with its line.
- **A path typed in can be 510 bytes.** Krome hands a text back in 512 and cuts a longer
  one silently (KR43); the face refuses one that may have been cut, and says so beside
  the edit. A longer path is queued from its row.
- **A string of 56 bytes or more** is written as a document (`krome_set_doc`); a
  `KromeSet` holds no more.

## See also

- [The slice 4 PRD](../../docs/PRDs/Blackwing-Slice-4-PRD.md) and its
  [notes](../../docs/design/slice-4-notes.md): what the face is for, and what building
  it found.
- [blackwing](../blackwing/README.md): which face is up, and the loop under it.
- [blackwing_player](../blackwing_player/README.md), [blackwing_library](../blackwing_library/README.md):
  the tables it reads and the commands it emits.
- `modules/krome/README.md` in the engine's checkout: screens, the model, markup.
