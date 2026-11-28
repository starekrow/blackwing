# blackwing_media

The OS's media surface ([`media_platform`](../media_platform/README.md)) joined to the
busses. It knows the player and the library; neither knows it. Application glue, as
[`blackwing_glue`](../blackwing_glue/README.md) is.

## What it does

**In.** What the OS asks becomes what a face would emit: `PlayerPlay`, `PlayerPause`,
`PlayerStop`, `PlayerNext`, `PlayerPrevious`, `PlayerSeekBy`, `PlayerSeek` (a place in the
track heard), and `PlayerSetParam` on the gain stage for a volume (0 to 1 as heard, so
20·log10 of it in decibels, -100 for 0). Play/pause is decided by the transport's state.
MPRIS's `Quit` ends the run; `Raise` is `MediaRaise`, which `blackwing_window` answers.

**Out**, all in DAC time:

- **The track**, on `PlayerTrackStarted`: its file's name at once, then the library's
  title, artist, album and number when its row comes (a window over the one path, as the
  face's queue titles are), then its art when the art's thread has looked.
- **The state and the place**, on a change, on a seek (`PlayerSeeked`, which is `Seeked`
  on the bus), and quietly every five seconds while playing, since the DAC's clock and the
  wall's drift apart. Never per tick: the OS works the position out from the rate.

**The art**: an image beside the file (`cover`, `folder`, `front`, `album`, `albumart`;
`.jpg`, `.jpeg` or `.png`; any case), else the front cover in its tags (else the first
picture), read from the file's first 16 MB and written once to `<cache>/art/`. On a thread
of the module's own, which wakes the loop with its answer.

**Keep-awake** while the transport is playing or starting, let go five seconds after it
is not (`awake_linger`): a pause between tracks is not a pause.

**Sleep**: the machine about to sleep is `MediaInterrupted`; playing, the transport is
paused first, and once it is the machine is let sleep. Waking is `MediaResumed`. Nothing
resumes on its own: the device is opened again by the next play.

## Using it

Its engine name is `media`. Off unless `media_configure` asks for it: `blackwing` asks for
a sitting (a window, or `library`) and for `play`, and never for a script, `help`, or a
run on the capture device, so a test owns no name on the desktop's bus. `--no-media` says
no.

| name | kind | what |
|---|---|---|
| `MediaRaise` | event | the OS asked for the window |
| `MediaInterrupted`, `MediaResumed` | events | the machine going to sleep, and back, with the reason in words |
| `media.status` | resource | open, awake and how, sleep heard, playing, the commands and reads, the name owned, the title and art shown, why there is none |
| `media status` | command | the same, in words |
| `media_find_art(path, art_dir, out, cap)` | function | the art for a file, as above; blocking |

## Tests

`kormake test blackwing_media`: the module over the player on the capture device and the
library, through a private `dbus-daemon`. An MPRIS client plays from the OS and hears the
track by its name with the cover beside it, the state, a seek, and sets the volume to the
gain stage's decibels; stand-ins for the portal and logind see keep-awake held while it
plays and let go after the linger, and send the machine to sleep while it plays, which
pauses it before the delay is let go; nothing resumes on waking; `Raise` and `Quit`. And
the art in an ID3v2 tag's front cover, written once.

## See also

- [The slice 5 PRD](../../docs/PRDs/Blackwing-Slice-5-PRD.md), "The media surface", "Keep-awake", "Interruptions, on a desktop".
- [blackwing_window](../blackwing_window/README.md): closed while playing, and raised.
- [modules](../README.md): the other modules and how they depend on each other.
