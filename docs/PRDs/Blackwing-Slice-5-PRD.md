# Blackwing Slice 5 — The system

Nov 27, 2026 · reviewed, its open questions answered

## Goal and exit criteria

Slices 1 to 4 built a player that plays on one desktop, into one device, picked at start,
and that nothing outside its own window can reach. Slice 5 puts it in the operating
system. The [master PRD](Blackwing-Master-PRD.md) gives the bar: the media keys and the
OS's now-playing surface work on Linux, Windows and macOS, and pulling the DAC pauses.

Five things are built: the devices, as the player's data (every output there is, a
choice among them, and what happens when one comes or goes); `media_platform`, the
OS's media surface (keys and remote commands in, what is playing out, the machine kept
awake, interruptions); the Windows and macOS backends of both platform modules;
output alone on a device, on every OS, so the indicator can read *identical* on real
hardware, with the resampler for a device that cannot take the track's rate; and the
first runs of the player through real hardware since slice 2, which slice 4 never made.

The risk is new again, and it is the audio path's. Every slice so far has played into
PipeWire on one machine, where PipeWire's data thread arrives realtime on its own and
the device never leaves. Two new backends are two new callbacks on threads Blackwing
has not run on, a device that vanishes is a callback that never comes, and a media
surface is a thread of the OS's calling into a process that must not let it near the
audio. So the tests are the capture device made to vanish on cue, a sink made and
destroyed under the player on each OS, the slowest callback on each backend, and the
media surface driven by the OS's own clients.

It is done when:

- **The keys work.** On each of the three desktops, with the player headless and with
  `ui_dev` up and not focused: the keyboard's play/pause, next, previous and stop, and a
  Bluetooth headset's buttons where there is one, do what they say. A key reaches the
  player as the same command a face emits, and is heard by slice 2's numbers plus what
  the OS took to deliver it, which the notes record.
- **The OS shows what is playing.** The title, artist, album, length, position and
  state, and the art where there is any, in GNOME's and KDE's media controls, Windows'
  media flyout and lock screen, and macOS's Now Playing; seeking from there seeks. A
  track change shows within a second of being heard (DAC time, B3), not when it was
  decoded.
- **Still is still free.** Paused with the media surface live: no wakeups added by it,
  counted over ten seconds against the same run with it off. Playing, it costs at most
  a message per state change and per seek, not one per tick: the OS is told the position
  and the rate, and works out the rest itself.
- **The devices are data.** Every output device the OS offers is a row of
  `player.devices`, with its name, what it is called, whether it is the default, and
  what it takes. Any one is chosen by a command, or "the default" is followed. A test
  lists and chooses on each OS, and a script does the same.
- **Pulling the DAC pauses.** A USB DAC pulled while playing: within a second the
  transport is paused at what was heard (DAC time), the device's row says it has gone,
  and the reason is in words. Nothing plays out of another device unasked. Plugged back,
  it is a row again; one press plays from where it was.
- **Headphones out is the DAC pulled.** A jack or a Bluetooth headset that goes away
  pauses the same way, where the OS says it went.
- **A device that comes is not a surprise.** A new default (a DAC plugged in, a headset
  connected) moves a player following the default to it without stopping, as a phone
  does; one that was chosen by name stays where it was. Either is an event.
- **No output at all is survived.** Started with no device, or with every one pulled: the
  library browses, the queue edits, play says why it cannot, and a device that appears
  is used.
- **Each backend's callback is clean.** On each OS: on the OS's realtime class (PipeWire's
  data thread; MMCSS "Pro Audio" on Windows; the HAL's IO thread on macOS), no
  allocation, lock or log, checked by the dev build's realtime marker; the slowest
  callback and the largest delay in the notes. Positions follow the device's reported
  latency on each, a Bluetooth headset's included.
- **The machine stays awake while playing**, and sleeps when paused: on each OS, an idle
  timeout shorter than the album does not stop it, and the hold is let go within a few
  seconds of pausing. The display is not held.
- **Sleep is an interruption.** The machine suspended mid-track wakes paused at what was
  heard; the device is opened again; one press plays.
- **Alone, it is exact.** On each OS a device opened alone (ALSA `hw:`, WASAPI
  exclusive, the HAL with the device's rate set and hog mode) at a rate it takes plays a
  FLAC file with `exact` set and the indicator reading *identical*, its word at least
  the file's depth, a 16-bit file in a 24-bit word padded and not dithered (BR22). At a
  rate it does not take, the resampler converts and dithers and the indicator says
  *modified* and why. Let go, the device is the OS's again, and shared programs play.
- **Slice 1 to 4's tests pass on Windows and macOS**, headless, into the capture device,
  and `ui_dev` comes up on both.
- **It is heard.** The hour (owed since slice 1) and the three disks with the face up
  (owed since slice 4) run through a real device on Linux, with no underrun; a real
  album plays end to end on Windows and on macOS.
- Numbers, in the notes: each backend's slowest callback, largest delay and period; a
  key's time to the player and to the ear; a pull's time to paused; what the media
  surface costs still and playing; memory and CPU playing on each OS.

## What the code has

- **`hifi_platform`** (`device.h`): open a device by name at a format, a callback, the
  delay to the ear, hold and close. Two devices: `"capture"` and PipeWire's `"default"`.
  No list of devices, no way to target one, no word when one goes. `HifiDeviceOpened`
  says `shared` and nothing of exclusive.
- **`hifi`**: the device is named once, in `HifiConfig`. A format change between tracks
  drains the lead and opens the device again. The render thread notices a device that
  has stopped calling back ("a daemon gone") only by its naps growing.
- **`blackwing_player`**: `PlayerConfig::device`, fixed before `engine_init`.
  `player.output` says open, shared, exact, the format and the lead, and not which device.
  The master PRD's sketch has `hifi.devices`, `PlayerSelectDevice` and
  `HifiDeviceChanged`; none is built, and slice 1 found that `hifi` has no busses, so the
  player publishes.
- **The session** keeps the queue, the place, the chain and a face's part. Not the
  device.
- **`media_platform`**: a page and a `module.lua` naming `core` and `platform`.
- **`blackwing`** with a face ends the run when the window closes (slice 4, decided).
  Slice 4 left "a player that goes on playing with no window" to this slice.
- **Linux only.** Every suite has run on one Ubuntu machine. Nothing has been built for
  Windows or macOS.
- **This machine**: PipeWire 1.6, a USB DAC, HDMI and the built-in analog output as three
  sinks; `libsystemd` 259 (sd-bus) and ALSA's headers present; GNOME, whose media keys
  and controls speak MPRIS.
- **Korvid**, at `5d6d000` (Nov 26): its slice 13 (several windows, shaped and placed:
  KR39 to KR42) and Krome's slices 7 and 8 (KR28 to KR37, for skin import) since slice 12.
  Nothing Blackwing calls changed shape: with kormake's one bug worked round (below),
  the build and all ten suites pass. Every window event gained a `window` field, and
  `HostWindow window` became `windows[]`, which `blackwing_window` and `ui_dev` read.
  For this slice the engine has:
  - **Windows and macOS builds**, each on its own host (MSVC through `bootstrap.bat`;
    Apple clang), no cross-compiling; `kormake check windows-dev` resolves the graph
    anywhere. `.mm` sources and `frameworks` for externals. No sanitizers on Apple's
    toolchain.
  - **The audio class** (KR7): `os_thread_enter_audio_class` is MMCSS "Pro Audio" on
    Windows and the time-constraint policy on macOS.
  - **A window's place** (KR42): `HostWindow::position`, `move_to`, `display_bounds`,
    which is most of KR44.
  - **Nothing** for D-Bus (core says it does not speak it), WASAPI, COM, SMTC, CoreAudio
    output on the desktop (`platform_audio` is SDL3 there), audio hot-plug, keep-awake,
    or the machine's sleep. KR45 is not done.

## What earlier slices left

Reviewed for this draft, slice by slice. Each is taken here, left with a reason, or a
fix to the documents.

| owed | from | here |
|---|---|---|
| **Blackwing does not build with Korvid's kormake today.** Korvid's `bff599d` added `test_files`, and kormake checks each one against the project's root, not the engine's (`tools/kormake/scripts/emit.lua`, `c.base_dir` where `c.root_dir` is meant), so a project that links `krome` fails before compiling: `patherizer: test_files names 'modules/patherizer/tests/references', which is not in the source tree` | Korvid, Nov 26 | **fixed** in Korvid's checkout, Nov 27 (not yet committed there): the two lines check against `c.root_dir`. The build and all ten suites pass |
| The hour, through a real device, on each of the three disks | slice 1, again in 2, 3, 4 | **taken**, on Linux at the close (milestone 8), with the face up. Owed four times; this slice is the one about real devices |
| The three disks with the window as the load, a real device, a resize through a run (BD22) | slice 4 | **taken**, with the hour. The resize is by hand |
| Ninety seconds with the scan as the load, through a real device | slice 3; slice 4 ran it into the capture device, the window mostly hidden | **taken**, with the hour |
| PipeWire played into at all with the face up | slice 4 ("PipeWire was not played into once") | **taken**: milestone 1 is through PipeWire |
| ALSA `hw:`: the path that can read *identical* on Linux hardware | slice 1's Open: "slice 5 or P1" | **taken** (milestone 7), with exclusive output on the other two |
| The resampler (B11) | slice 2 proposed slice 5 | **taken** (milestone 7): a device opened alone at a rate it cannot take needs it |
| BR22, the device at its widest word | slice 2, P1 | **taken** (milestone 7): an unshared device is where it first matters |
| KR10, the timed wait: `hifi`'s render thread still naps (`hifi.cc`) | slice 2 | **taken** (milestone 0). The engine has had it since its slice 10, and a device that stops calling back is this slice's business |
| KR15 and KR16: the corpus test's `mkdir` and the watcher's link filter (`watch.cc`, `Links`) may go | slice 3 | **taken** (milestone 0): small, and the workarounds are code to port to two more OSes |
| KR24: the index into the cache directory; the store unused | slice 4 | **taken** in part (milestone 0): the index moves, read from where it was once. The session stays a file |
| KR44: a window's place, a list's place | slice 4 | **taken in part** (milestone 0): Korvid's KR42 gives a window's position and `move_to`, so the session puts the window back where it was. A place given at open, a list's scroll and a user directory before the engine are still asked |
| KR45: a present that blocks holds the ticks | slice 4, "P0 for slice 5" | **asked again**: Korvid has not taken it, and Windows and macOS are two more renderers that may block. It blocks milestone 5's face on Windows and 6's on macOS only if they are seen to block |
| An override file set aside is not published | slice 4 | **taken** (milestone 0): a failure without a place on screen breaks B23 |
| BL25, art | slice 3, P1 | **taken in part**: every OS's now-playing surface wants a picture. The image beside the files is a path to hand the OS; one inside the tags is written to the cache directory once, and that path handed over |
| BD24, the queue reordered by dragging | slice 4, P1 | **left** to slice 6, with the faces' design |
| An artist's tracks with no album, in a view | slice 4 | **left** to slice 6, with the views |
| A search matching every track: 32 ms for its first window | slice 3 | **left**: under a millisecond after, and nothing here leans on it |
| BL23, the background pass | slice 3, P1 | **left**: P1 |
| A ship build's numbers | slice 4 | **taken** at the close, on all three OSes |
| The master PRD's opening still says slice 1 is all there is, and its module drawing lacks the glue, the session, the script host and the window | documents | **fixed** with this draft's review |
| The Korvid changes page: its Status says the engine's slices 10 and 11 are not merged to `main` (they are, and 12 and 13 with them); KR28 to KR37 and KR39 to KR42 say "nothing built" (built, in Krome's slices 7 and 8 and the engine's 13); KR44 says nothing (in part); KR7 says "not needed" (the WASAPI thread needs it) | documents | **fixed** with this draft's review |

## The shape

### Where things are

| module | this slice |
|---|---|
| `hifi_platform` | the device list and its watch; a device by name; a device that goes; WASAPI on Windows, CoreAudio on macOS |
| `hifi` | a device lost mid-stream: what was heard, kept; opened again on another, from there |
| `blackwing_player` | `player.devices`, `PlayerSelectDevice`, `PlayerDeviceChanged`; `player.output` says which device; pausing on a loss |
| `dbus` | new: a D-Bus client of Blackwing's own, Linux only, for `media_platform` and `hifi_platform` |
| `media_platform` | the OS's media surface as a library, per OS: MPRIS, SMTC, Now Playing; keep-awake; sleep and wake |
| `blackwing_media` | new: the customer that joins `media_platform` to the busses: what is playing out, commands in |
| `blackwing_session` | the device chosen, kept |
| `ui_dev` | the devices on the Output panel; a Media panel: what the OS was told and what it sent |
| `blackwing` | the Windows and macOS builds; what a closed window means now |

### The devices are the player's data

`hifi_platform` gains a list and a watch; the player publishes them, as it does the
output. The policy (pause on a loss, follow the default) is the player's, not the
backend's: `hifi_platform` reports, `hifi` keeps what was heard, the player decides.

- **`player.devices`**, a ktable, a row per output the OS offers: the backend's own name
  for it, stable across runs where the OS has one (PipeWire's `node.name`, the Windows
  endpoint id, the CoreAudio UID); what it is called; the default or not; present or
  gone; the rates, channels and sample formats it takes, as far as the OS says; shared
  or able to be opened alone. A row that goes stays, marked gone, until the run ends, so
  a face can say what was pulled.
- **`PlayerSelectDevice`**, by that name, or `default` to follow the default. The lead is
  rendered again for the new device from what was heard: a jump, not a gap of five
  seconds.
- **`PlayerDeviceChanged`**: added, gone, back, the default moved, lost while playing,
  each with the device's name, the reason in words and `seq`. The master PRD's sketch
  called it `HifiDeviceChanged`; it is the player's for the reason `player.output` is.
- **`player.output`** says which device is open, whether it is the default being
  followed, and why none is when none is.
- **The session keeps the choice**, by name. A device chosen and absent at the next
  start is a preference of its own, `absent_device`: `default` (the default setting)
  plays on the default and says so, and moves back to the chosen one when it appears;
  `wait` plays nothing until it does. It is Blackwing's first setting, and lives in the
  engine's settings store (KR24's `platform_settings`), not in the session. A device
  lost while playing still pauses, whichever is set: that rule is about a pull, and
  this one about a start.

### When a device goes

A device that goes is a callback that stops coming, or an error from the OS on the way
in. Every OS says it differently and too late, so `hifi` does not wait to be told.

- **`hifi_platform` reports a device lost** from its own thread: PipeWire's stream
  state or its node removed; WASAPI's `AUDCLNT_E_DEVICE_INVALIDATED` or
  `IMMNotificationClient`; CoreAudio's `kAudioDevicePropertyDeviceIsAlive`. The report is
  a flag the render thread reads, nothing more.
- **`hifi` keeps what was heard.** The position is what the device's clock had passed at
  its last good callback, less its delay then: frames given to a device that has gone
  were not heard. The lead is not thrown away; the source ring still holds the music,
  so playing again is a jump from that frame.
- **The player pauses** and says so, in `player.transport` and in `PlayerDeviceChanged`.
  It never moves to another device on a loss: sound out of the laptop's speakers when
  the headphones come out is the thing this rule exists to prevent.
- **What a pull costs is bounded**: paused within a second of the OS knowing. The OS's
  own lag is in the notes.
- **A device that comes back** is a row present again. A player paused by losing it is
  not started by its return; a press is.

### When a device comes

- **Following the default, the player moves with it**, without pausing, through the jump.
  Plugging a DAC in, or a headset connecting, and the OS making it the default: the
  music moves to it, as on a phone.
- **A device chosen by name stays chosen.** A new default is an event and nothing else.
- **No device at all** is a state, not an error: `player.output` says none, play says
  why, and the first device to appear is opened if the default is being followed.

### The rate, shared and alone

The master PRD's P0 asks each device for the track's rate and gets out of the way when it
can (B11), and puts exclusive mode on every OS in P1 (B42). This slice settles what P0
means on each backend:

| | shared (the default, every OS) | alone |
|---|---|---|
| Linux | PipeWire, asked for the track's rate (`node.rate`); the graph switches if its allowed rates let it, and resamples otherwise. Reads *modified* | ALSA `hw:`, PipeWire told to let go through its device reservation |
| Windows | WASAPI shared, event-driven, at the mix format; Windows converts. Reads *modified* | WASAPI exclusive, event-driven, at the track's rate where the device takes it |
| macOS | the HAL, the device's nominal rate left as it is; CoreAudio converts. Reads *modified* | hog mode, the device's nominal rate set to the track's and its physical format to an integer one |

Both are built (decided Nov 27). Shared stays the default: it is what every other program
on the machine expects, and it is what makes the keys and the flyout behave. Alone is a
choice per device, kept in the session, and is the indicator reading *identical* on real
hardware for the first time. B42 (exclusive on every OS) moves from P1 into this slice;
its hardware volume, per-device settings and DSD stay in P1.

- **Alone is asked for, and may be refused.** Another program holds the device, the OS
  forbids it, the device takes no format the file fits: `player.output` says shared or
  none, and why, in words. A refusal never plays silence.
- **The format is chosen for the file**: its rate if the device takes it, else the
  device's nearest, through the resampler; the narrowest word that holds the file's
  depth, else the widest the device has, padded (BR22).
- **Gapless across a rate change** reopens the device, as now; with the device alone, a
  rate change between tracks is a gap, which the master PRD already allows (B15).
- **Letting go is prompt.** Paused alone for a minute, the device is closed so other
  programs can sound; play opens it again, as after a sleep. Where the OS can say that
  another program wants it (PipeWire's reservation has `RequestRelease`), it is let go
  at once, paused or not, and the player pauses as on a pull. The minute is a setting,
  `exclusive_hold`. Why a minute: a reopen costs a fraction of a second and, on many
  DACs, a relock of the clock, often with a click or a muted moment, so a pause of a few
  seconds (a word with someone) should not pay it; while held, no other program can
  sound on the device (a call, a notification, a video), so a pause to take a call
  should not wait five minutes for it. Players differ here, and many hold until stop;
  the notes record what a reopen costs on each OS's backend and DAC.
- **On Linux, alone is ALSA `hw:`**, which PipeWire must let go of first. The backend asks
  through PipeWire's own reservation (the `org.freedesktop.ReserveDevice1` name on the
  session bus, which WirePlumber honours), and gives the device back the same way.
- **On macOS, alone is hog mode and the nominal rate**, set back to what it was on close.
  A device left at another rate by a crash is a finding for the notes.

### The resampler

Libsoxr is LGPL, and the master PRD's third-party code is permissive. So `hifi` gets its
own, as a stage: polyphase windowed sinc in f64, with the passband, the stopband and the
length set for 44.1 to 48 kHz and its multiples, and TPDF dither at the end of the chain
as now. A recut runs it like any stage; its run-in is its filter's length. Its response
is measured in a test as the equalizer's is (slice 2), and published like any stage's
parameters. It is written, not taken (decided Nov 27), so that what it does to the
audio, and how precisely, is ours to say and to measure.

### The media surface

`media_platform` is to the OS's media surface what `hifi_platform` is to its audio: a
library with a small C++ interface and a backend per OS, which knows nothing of the
player. A second module, `blackwing_media`, is its customer on the busses, as `ui_dev`
is the player's.

- **Why two.** The surface must reach the library for titles (a queue row has a path),
  and the player must not know it exists (the master PRD). A module that depends on both
  is application glue, which belongs above them, as `blackwing_glue` does. The OS half
  stays something another project could link.
- **Out**: what is playing (title, artist, album, length, art), the state, and the
  position with its rate and the time it was true. Sent when one of them changes, and on
  a seek; never per tick. Every OS extrapolates a position from a rate.
- **In**: play, pause, play/pause, stop, next, previous, seek to, seek by, and the
  volume where the OS has it (MPRIS does). Each becomes the command a face would emit.
  The surface keeps no state of the player's that it could not read again.
- **The OS's thread is not the audio's.** Each backend's calls in (D-Bus on its own
  thread, SMTC's on a WinRT thread, `MPRemoteCommandCenter`'s on the main run loop) are
  queued and taken up in a tick. Nothing it does touches `hifi`.
- **Time is the DAC's.** A track change is sent when `PlayerTrackStarted` is, not when
  the next item was opened.

**Linux: MPRIS.** `org.mpris.MediaPlayer2.blackwing` on the session bus (with an
instance suffix if a second Blackwing has the name), the `MediaPlayer2` and
`MediaPlayer2.Player` interfaces. `Position` is a property the shell reads when it wants
it, and `Seeked` a signal, which is what lets a still player cost nothing. The bus is
reached through `dbus` (below). `Raise` brings the window up when there is one; `Quit` ends the run.

**Windows: SMTC.** The System Media Transport Controls, through WinRT from C++. A desktop
program reaches them through a window (`ISystemMediaTransportControlsInterop`) or
through a `MediaPlayer`'s command manager with none. Headless, the second (Open). The
art is a file stream.

**macOS: Now Playing.** `MPNowPlayingInfoCenter` and `MPRemoteCommandCenter`, from
Objective-C++, on the main run loop. macOS gives the keys to the app it thinks is
playing, so the surface says so whenever it plays. Blackwing ships as an app bundle
(decided Nov 27), so it is built as one on macOS from milestone 6, and the surface is
tested from the bundle; `blackwing` run from a shell is the same executable inside it.

### D-Bus

Every Linux piece of this slice that talks to the desktop does it over the session or
the system bus, so Blackwing speaks D-Bus itself (decided Nov 27): a client of its own,
`dbus`, with no library under it. libdbus is permissive (AFL 2.1) but verbose and
low-level; sd-bus and GDBus are LGPL, and sd-bus is systemd's only. The protocol is
specified whole and has not moved in a decade ([the D-Bus
Specification](https://dbus.freedesktop.org/doc/dbus-specification.html)), and what
Blackwing needs of it is small.

**Its customers**, four, which is why it is a module and not part of one of them:

| customer | bus | what |
|---|---|---|
| `media_platform`: MPRIS | session | a name owned; methods answered; properties and `PropertiesChanged`; the `Seeked` signal |
| `media_platform`: keep-awake | session, system | the desktop portal's `Inhibit`; logind's `Inhibit`, which answers with a file descriptor |
| `media_platform`: sleep | system | logind's `PrepareForSleep` signal, and a delay inhibitor so the pause lands before the machine sleeps |
| `hifi_platform`: alone | session | `org.freedesktop.ReserveDevice1`, so PipeWire lets go of a device for ALSA `hw:` and has it back |

**What it is:**

- **A connection**: the address from `DBUS_SESSION_BUS_ADDRESS` (its `unix:path=` and
  `abstract=` forms, and their escaping), else `$XDG_RUNTIME_DIR/bus`; the system bus at
  its fixed path. Authentication is `EXTERNAL` with the uid, and `NEGOTIATE_UNIX_FD`.
  `Hello` first.
- **The wire format**: marshalling and parsing of every type, both byte orders, every
  value aligned from the message's start (an array's length not counting the padding
  before its first element; an empty one still padded), the header's fields, the
  spec's limits on depth, size and signature length.
- **Messages**: calls with their replies matched by serial, errors as replies, signals
  sent and received through match rules, a name requested and released, file
  descriptors passed in and out (`SCM_RIGHTS`).
- **Objects of our own**: a path with interfaces, their methods, properties through
  `org.freedesktop.DBus.Properties` (`Get`, `GetAll`, `Set`, `PropertiesChanged`), and
  `Introspectable`'s XML, which no client needs and `busctl` and D-Spy want.
- **A thread of its own** per connection, which owns the socket. Customers post to it
  and are posted to; nothing of theirs runs on it. It sleeps on the socket and on a
  wake, so an idle bus costs nothing.
- **Hostile input.** Everything on the bus comes from another process. The parser
  checks every length, offset, signature and nesting before it reads, refuses a message
  it does not like and keeps the connection, and is fuzzed.

**What it is not**: a bus daemon, a code generator, a binding of every interface, or
anything on Windows or macOS. It answers to the parts of the spec its four customers
use; the rest is refused in words.

**MPRIS's own rules**, which the spec says and the shells hold to: `mpris:trackid` is an
object path, lengths and positions are microseconds as `int64`, `xesam:artist` is a list,
the art is a `file://` URI, `Position` changes are never signalled, and GNOME shows a
player only with `CanControl` true, a status and some metadata. A second Blackwing owns
`org.mpris.MediaPlayer2.blackwing.instance<pid>`. A Flatpak, later, grants the name in
its manifest.

### Keep-awake

Held while the transport is playing, let go a few seconds after it is not (a pause
between tracks is not a pause). The system's idle sleep only; the display may sleep.

| OS | how |
|---|---|
| Linux | the desktop portal's `Inhibit` (suspend), and logind's `idle:sleep` inhibitor where there is no portal |
| Windows | a power request, `PowerRequestSystemRequired` |
| macOS | `IOPMAssertionCreateWithName`, `PreventUserIdleSystemSleep` |

### Interruptions, on a desktop

The master PRD has interruptions as events; on a phone they are calls. On these three
they are:

- **The machine going to sleep**: logind's `PrepareForSleep`, Windows' suspend broadcast,
  macOS's will-sleep. The player pauses before it sleeps, so it wakes paused at what was
  heard, and opens the device again on the next play.
- **The device taken**: another program opening it alone (WASAPI exclusive, hog mode).
  The same as the device going.
- **A headset's own buttons** arrive as keys, through the OS, and are not interruptions.

Each is `MediaInterrupted` and `MediaResumed` out of `blackwing_media`, with the reason
in words. Nothing resumes on its own after one.

### The window closed

Slice 4 decided that closing the window ends the run, and left a player that plays on
without one to this slice. With MPRIS's `Raise` and the flyouts there is now a way back
to a window that is not there, so slice 4's rule changes (decided Nov 27):

- **Closed while playing, it plays on**, headless, with the OS's surface live: the keys,
  the flyout, Now Playing. The session is saved as at exit, and the face's part with it.
- **Closed while paused or stopped, it ends the run**, as before. So does the queue
  ending with no window, and MPRIS's `Quit`. A program with nothing to play and nothing
  on screen does not linger.
- **The window comes back** from the OS's surface: MPRIS's `Raise` on Linux; the Dock's
  icon on macOS (the bundle's reopen); on Windows the flyout opening the program, which
  wants the program to have an identity of its own (an AppUserModelID), set from the
  build. The face comes back as it was left, from the session's part.
- **A tray icon or a menu-bar player** is still slice 6's: a face designed for it.

### Windows

- **The build.** `blackwing` and every module it links, for `windows-dev` and a ship
  target, with whatever toolchain Korvid builds Windows with. Every suite runs headless
  into the capture device.
- **`hifi_platform`'s backend** (`src/windows/wasapi.cc`): WASAPI, shared, event-driven,
  on a thread of its own in MMCSS's "Pro Audio" class (Korvid's
  `os_thread_enter_audio_class`, KR7, which slice 1 did not need). The delay from
  `IAudioClock` and the stream's latency. The list and the watch through
  `IMMDeviceEnumerator` and `IMMNotificationClient`, whose calls come on the OS's thread
  and are queued.
- **`media_platform`'s backend** (`src/windows/smtc.cc`), a power request, and the
  suspend broadcast.
- **The machine**: this one, booted into Windows, with the same USB DAC, HDMI and
  analog outputs as Linux, so the two backends are compared on the same hardware. It
  cannot run Linux at the same time: milestone 5 is a stretch at a time, not beside
  milestones 1 to 4.
- **What is different**: paths are UTF-16 at the OS, and every path in Blackwing's events
  is UTF-8 of any length (slice 4); the library's watcher, links and case-insensitive
  names; the session and index under the user's AppData through KR24.

### macOS

- **The build**, for `macos-dev` and a ship target, with Objective-C++ for the media
  surface. Every suite runs headless into the capture device.
- **`hifi_platform`'s backend** (`src/macos/coreaudio.cc`): the HAL directly (an IO proc
  on the device, `AudioDeviceCreateIOProcID`), not an audio unit, so the device and its
  format are ours to name. The delay from the device's and the stream's latency, its
  safety offset and its buffer. The list and the watch through the HAL's property
  listeners on the system object and each device.
- **`media_platform`'s backend** (`src/macos/nowplaying.mm`), the power assertion, and
  `NSWorkspace`'s sleep notifications.
- **The bundle**: `Blackwing.app`, its `Info.plist` and bundle id, its assets inside it,
  built by kormake if its packaging has a macOS bundle and asked of the engine if not.
  Unsigned, or signed for this machine only: signing for others is still future work.
- **What is different**: the main thread's run loop is the OS's, and the remote commands
  want it; the library's watcher on FSEvents.
- **The machine**: an M3 MacBook Pro, its own output and whatever DAC is to hand.

### How it is tested

- **The capture device vanishes on cue.** `hifi_capture_unplug` and `hifi_capture_plug`
  make it go and come back between two pulls, so every rule above is tested headless and
  exactly: the position at a pull, paused, no other device opened, one press plays, the
  default followed.
- **A real device made and destroyed under the player.** On Linux a null sink loaded and
  unloaded (`pactl load-module module-null-sink`), on macOS an aggregate device created
  and destroyed (`AudioHardwareCreateAggregateDevice`), on Windows by hand. The list, the
  watch and a loss mid-stream on each OS's own notifications.
- **The surface driven by the OS's own clients.** On Linux a test that is an MPRIS
  client on the session bus: it calls `PlayPause`, reads `Metadata` and `Position`, and
  checks the player against the capture device. On Windows a test through the session
  manager, SMTC's client side. On macOS what `MPNowPlayingInfoCenter` holds is read back;
  the keys by hand.
- **`dbus` against a bus of its own.** A `dbus-daemon` started by the test, private to
  it: calls, replies, errors, signals, names, properties and a file descriptor, against
  a peer written in the test and against `busctl`. The parser is fuzzed with messages
  made wrong in every field, and none is read past its end.
- **By counters**: the loop's wakeups paused, with the surface on and off.
- **By hand, with a list**, on each OS: the keys, the flyout, a DAC pulled, headphones
  out, a headset connected, the lid closed and opened. Results in the notes.
- **The disk runs**, through the USB DAC, the face up.

## What the engine gets

To be written up as KR46 onward in
[Korvid-Changes-For-Blackwing.md](Korvid-Changes-For-Blackwing.md) once the review has
settled them. The rule the draft uses: what any program on the engine would want goes to
the engine; what is a music player's stays in Blackwing's platform modules.

- **KR45, again**: a present that blocks does not hold the ticks. A blocking present on
  Windows or macOS is the same stall as under XWayland.
- **KR46, the machine's sleep and wake** as events, on every platform: logind's
  `PrepareForSleep`, Windows' power broadcast, `NSWorkspace`'s notifications. A game
  wants them too. Wanted by milestone 3; the workaround is each backend of
  `media_platform` listening for itself.
- **KR47, keep-awake**: a hold on idle sleep, with or without the display, on every
  platform. A game wants the display kept; a player wants only the machine. Wanted by
  milestone 3; the workaround is `media_platform`'s own, as in the table above.
- **KR48, the main thread's run loop for Apple's frameworks**: a module can have a block
  run on the main thread's `CFRunLoop`, and the run loop is turned while the engine owns
  the loop, headless as well as with a window. `MPRemoteCommandCenter` wants it. Blocks
  milestone 6 unless the headless loop on macOS already turns it.
- **KR44's rest**: a place given at open, a scroll box's place, a user directory before
  the engine starts.
- **KR7, checked** on the WASAPI thread and the HAL's: not a change, a use.

Not asked, and kept in Blackwing: D-Bus (core says it does not speak it, and it is
Blackwing's own `dbus`, which the engine could take later if a game wants the portal); WASAPI, CoreAudio and audio hot-plug (`hifi_platform` exists so
that Blackwing's output is not `platform_audio`'s); SMTC and Now Playing.

## Out of scope

- **The faces**: `ui_wsz`, `ui_clean`, `ui_compact`, the visualizer and `hifi.meter`.
  Slice 6. `ui_dev` shows what this slice adds and no more.
- **A tray icon or a menu-bar player.** Slice 6.
- **Hardware volume, per-device settings, DSD over PCM** (the rest of B42, P1).
- **iOS.** The iPhone is for Korvid's ports; a phone backend of `media_platform` is not
  this slice's.
- **File associations, single instance, drag and drop, enqueue from the command line**
  (B41, P1). MPRIS's `OpenUri` is not offered.
- **Shuffle and repeat.** MPRIS's `Shuffle` and `LoopStatus` are not offered.
- **Signing, notarization, installers.** Future work. The macOS bundle is built here, as
  the shape Blackwing ships in.
- **Mobile.** The surface is shaped so a phone's is a fourth backend, and nothing more.
- Left from before, with the reasons above: BD24, BL23, an artist's tracks with no
  album, the first window of a search of everything.

## Requirements

| ID | Requirement | Priority |
|---|---|---|
| BP1 | `hifi_platform` lists the output devices, watches them come and go, and opens one by name, on every backend | P0 |
| BP2 | `player.devices`, `PlayerSelectDevice` (by name, or the default followed), `PlayerDeviceChanged`; `player.output` names its device | P0 |
| BP3 | A device lost while playing pauses at what was heard, within a second of the OS knowing; no other device is opened | P0 |
| BP4 | A new default is followed without pausing when the default is chosen; a device chosen by name stays chosen | P0 |
| BP5 | No output device at all is a state the player runs in; the first to appear is used | P0 |
| BP6 | The capture device can be pulled and plugged on cue | P0 |
| BP7 | `media_platform`: what is playing, the state and the position out; the transport commands in; on Linux, Windows and macOS | P0 |
| BP8 | `blackwing_media` joins it to the busses; the OS's calls are taken up in a tick and never reach the audio path | P0 |
| BP9 | A track change reaches the OS in DAC time; the position is sent with its rate, never per tick | P0 |
| BP10 | Keep-awake while playing, on every OS; the display is not held | P0 |
| BP11 | Sleep is an interruption: paused before it, paused after it, the device opened again | P0 |
| BP12 | The WASAPI backend: shared, event-driven, MMCSS, its delay, its list and watch | P0 |
| BP13 | The CoreAudio backend: the HAL's IO proc, its delay, its list and watch | P0 |
| BP14 | Blackwing and every suite build and pass on Windows and macOS, headless, into the capture device; `ui_dev` comes up on both | P0 |
| BP15 | Every backend's callback on the OS's realtime class, with nothing that allocates, locks or logs | P0 |
| BP16 | Art for the now-playing surface: the image beside the files, or one in the tags written once to the cache | P0 |
| BP17 | The session keeps the device chosen | P0 |
| BP18 | `ui_dev`: the devices and a choice on the Output panel; a Media panel | P0 |
| BP19 | The catch-up: KR10's timed wait in `hifi`; KR15's and KR16's workarounds gone; the index in the cache directory; an override file set aside published; the window put back where it was (KR42) | P0 |
| BP20 | The disk runs through a real device on Linux, the face up: the hour, the three disks, the scan as a load | P1 |
| BP21 | Output alone on every OS (ALSA `hw:`, WASAPI exclusive, hog mode and the nominal rate), asked per device, refused in words, let go when paused | P0 |
| BP23 | The resampler, a stage in `hifi`: windowed sinc in f64, its response measured | P0 |
| BP24 | The device at its widest word: a narrower file padded, not dithered (BR22) | P0 |
| BP25 | `Blackwing.app` on macOS, with the media surface tested from it | P0 |
| BP29 | Closed while playing, it plays on with the OS's surface live, and the surface brings the window back; closed otherwise, it ends | P0 |
| BP30 | Settings `absent_device` (default: play on the default) and `exclusive_hold` (default: a minute), in the engine's settings store | P0 |
| BP26 | `dbus`: a client of the session and system buses with no library under it; calls, replies, errors, signals, names, properties, introspection and file descriptors | P0 |
| BP27 | `dbus`'s parser takes hostile input: every length checked, fuzzed, a bad message refused and the connection kept | P0 |
| BP28 | `dbus` on a thread of its own per connection; an idle bus costs no wakeups | P0 |
| BP22 | Module pages for `dbus`, `media_platform` and `blackwing_media`; `hifi_platform`'s page for three OSes; numbers in `docs/design/slice-5-notes.md` | P1 |

## Milestones

Each leaves the tree building and its tests passing, on every OS it has reached.

0. **The catch-up** (BP19). What earlier slices left that is small and in the way: the
   render thread waits where it naps, two workarounds go, the index moves, a set-aside
   override is published. Korvid's changes since slice 12 are taken up here.
1. **Devices on Linux** (BP1 to BP6, BP17, BP30's `absent_device`). The list and the watch over PipeWire's
   registry and its default; a device by name; the loss, the return, the default
   followed; the capture device pulled on cue; the session's device. Through PipeWire,
   with the USB DAC pulled by hand.
2. **D-Bus** (BP26 to BP28). `dbus`, whole, against a private bus: the wire format,
   the connection on both buses, calls, signals, names, properties, file descriptors,
   the fuzzing. Proved by its first customer's smallest piece, a name owned with
   MPRIS's identity on the real session bus and seen by `busctl`.
3. **The surface on Linux** (BP7 to BP11, BP16, BP29). `media_platform` over MPRIS on `dbus`,
   `blackwing_media`, keep-awake, sleep, art. GNOME's controls and keys by hand; the test
   as an MPRIS client.
4. **`ui_dev`** (BP18). The devices, the choice, the Media panel. Small, and it is how
   milestones 5 and 6 are looked at on the other two.
5. **Windows** (BP12, BP14, BP15, and BP1 to BP11 there). The build first, every suite
   into the capture device; then WASAPI; then SMTC, the power request and suspend.
6. **macOS** (BP13, BP14, BP15, BP25, and BP1 to BP11 there). The same order, the
   bundle with the build.
7. **Alone** (BP21, BP23, BP24, BP30's `exclusive_hold`). The resampler first, on the capture device, which can
   be told to refuse a rate; then ALSA `hw:` (with `dbus` for the reservation), WASAPI
   exclusive and hog mode, each on its own OS's backend.
8. **Close** (BP20, BP22). The disk runs, the hour, the lists by hand on each OS, a ship
   build's numbers on each, the pages, the master PRD corrected where this slice proved
   it wrong.

Linux goes first in both halves because it is where the tests already run and the
design can be got right against a real bus and a real pull. `dbus` is a milestone of
its own because it is a protocol, tested on its own terms before anything leans on it;
it needs nothing of milestone 1, and can be built beside it. The face comes before the
other two OSes because it is how they will be looked at. Windows and macOS are
independent of each other: macOS on its own machine can be built beside anything;
Windows is this machine booted the other way, so it is built in stretches. Alone comes
after the three backends because each OS's exclusive path is a change to its backend.

## Decided

Nov 27, 2026, on the first draft.

- **Exclusive output is in this slice**, on all three OSes, with the resampler and
  BR22: building it now finds what else the backends need while they are being written.
  B42's exclusive mode moves from P1 to here.
- **The machines**: this one for Linux and, dual-booted, Windows; an M3 MacBook Pro for
  macOS.
- **Blackwing ships as an app bundle**, so the macOS build is one from the start.
- **Blackwing speaks D-Bus itself**: `dbus`, a module of its own with no library under
  it, for MPRIS, keep-awake, sleep and the device reservation. libdbus, sd-bus and GDBus
  were weighed (licences LGPL or verbose, or systemd's only); the protocol is specified
  whole and stable, and what is needed of it is small.
- **kormake's `test_files` bug is fixed in Korvid's checkout** (`emit.lua`, the paths
  checked against the engine's root), so Blackwing builds again.

- **The resampler is written**, in `hifi`, so its capabilities and its precision are
  ours to state.
- **An exclusive device is let go after a minute paused**, or at once when the OS says
  another program wants it; the minute is a setting.
- **SMTC without a window** through a `MediaPlayer`'s command manager, or a hidden window
  for the interop: whichever works headless and in a face alike.
- **A chosen device absent at start plays on the default** and moves back when it
  appears; waiting for it is a setting.
- **Closed while playing, it plays on**; the OS's surface brings the window back.
- **The numbers stand**: a second from a pull to paused; a second from heard to shown; a
  few seconds before keep-awake lets go.
- **Requirement IDs** are `BP`.

## Open

- **Where the settings are shown.** Two settings and no face for them: `ui_dev` shows the
  store as it shows a table, and a settings screen is slice 6's.
