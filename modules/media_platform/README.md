# media_platform

The operating system's media surface, as `hifi_platform` is its audio: a library with a
small C++ interface and a backend per OS, which knows nothing of the player. What is
playing, its state and its position go out; the transport's commands come in from the
media keys, the shell's controls and anything that speaks the OS's protocol. It also holds
the machine awake while asked, and says when the machine is about to sleep and has woken.
[`blackwing_media`](../blackwing_media/README.md) is its customer on the busses.

**Status:** Linux (MPRIS, keep-awake, sleep). Windows (SMTC) and macOS (Now Playing) are
slice 5's milestones 5 and 6; until then `media_open` says there is no surface.

## What it provides

`media.h`:

- `media_open(desc)`: the surface, named for the OS (`identity`, `desktop_entry`, `name`),
  saying whether it can raise a window and quit; `notify` is called (on the surface's
  thread) when there is a command or an interruption to take. Null, with why, where there
  is none.
- **Out**: `media_set_track` (title, artist, album, album artist, track number, length,
  the file and its art), `media_set_status` (the state; the position as of a time, with
  its rate; the volume; what can be done: play, pause, next, previous, seek),
  `media_seeked` after a jump. Nothing is sent to the OS that it does not show: a status
  that only moves the position is kept, not signalled, and the position is worked out
  from it when the OS asks.
- **In**: `media_next_command`: play, pause, play/pause, stop, next, previous, seek to (in
  a track), seek by, a volume, raise, quit.
- **Keep-awake**: `media_keep_awake(on)`. The machine's idle sleep only; the display may
  sleep.
- **Sleep**: `media_next_interruption` says Sleep before the machine sleeps and Wake after;
  `media_sleep_ready` says the caller has paused, so the machine may go. It goes a few
  seconds later regardless.
- `media_info`: the name owned, whether it is held awake and how, whether sleep is heard,
  the commands taken.

Every call may be made from any thread. The OS's calls are answered on a thread of the
surface's own, and queued for the caller's tick; nothing of the caller's runs there but
`notify`.

## Linux

Through [`dbus`](../dbus/README.md), Blackwing's own client.

**MPRIS** on the session bus: `org.mpris.MediaPlayer2.<name>`, or
`.instance<pid>` if a Blackwing already has it, at `/org/mpris/MediaPlayer2`, with
`org.mpris.MediaPlayer2` (`Raise`, `Quit`, `Identity`, `DesktopEntry`, `CanRaise`,
`CanQuit`, the schemes and types it plays) and `org.mpris.MediaPlayer2.Player` (the six
transport methods, `Seek`, `SetPosition`, `OpenUri` refused, and every property the spec
names but `LoopStatus` and `Shuffle`, which the player has not got). The spec's rules: the
track id is an object path (`/net/blackwing/track/<id>`), lengths and positions are
microseconds, `xesam:artist` is a list, the art and the file are `file://` URIs with what a
URI cannot carry escaped, `Position` is read and never signalled, and `Seeked` says a jump.
A `SetPosition` for another track is ignored, as the spec says.

**Keep-awake**: the desktop portal's `Inhibit` with the suspend flag (4), let go by
closing its request; where there is no portal, logind's `Inhibit("idle", ..., "block")`,
let go by closing its descriptor. Not logind's `sleep`: that would also hold off a suspend
the user asks for.

**Sleep**: logind's `PrepareForSleep` on the system bus, with a `delay` inhibitor held
from the start, let go when the caller has paused (or after three seconds) and taken
again on waking.

## Tests

`kormake test media_platform`: against a private `dbus-daemon` that stands for both buses.
An MPRIS client written on `dbus` reads what a shell reads first, hears the track and the
state change, reads the position moving with no signal sent, sends each command and sets
the volume; a second surface takes the instance name. Stand-ins for the portal and logind
see keep-awake taken and let go both ways, and send `PrepareForSleep`, seeing the delay
held while the caller pauses and let go after. Skipped where `dbus-daemon` will not start.

By hand, on a desktop: `blackwing` playing into a null sink, `busctl --user` reading the
name, its `Identity`, `PlaybackStatus` and a moving `Position`; `gnome-session-inhibit
--list` showing the portal's suspend inhibitor; `systemd-inhibit --list` the delay. GNOME's
own controls and keys are slice 5's close.

## See also

- [The slice 5 PRD](../../docs/PRDs/Blackwing-Slice-5-PRD.md), "The media surface".
- [blackwing_media](../blackwing_media/README.md): its customer.
- [modules](../README.md): the other modules and how they depend on each other.
