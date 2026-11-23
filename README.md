# Blackwing

A hi-fi local music player built on the [Korvid](../korvid) engine. It plays FLAC files
from the command line through PipeWire on Linux, or into a capture device that keeps what
it was given.

- [docs/README.md](docs/README.md): where the documents are.
- [docs/PRDs/Blackwing-Master-PRD.md](docs/PRDs/Blackwing-Master-PRD.md): what is being
  built, in what order.
- [modules/README.md](modules/README.md): the modules.

## Building

Korvid is expected beside this checkout, at `../korvid` ([korvid.config](korvid.config)
says so), at a version whose `kormake` is 1.2 or newer.

On Linux, the PipeWire development package, for the headers and `pkg-config` file that
`hifi_platform` builds against. On Debian and Ubuntu:

```bash
sudo apt install libpipewire-0.3-dev
```

`kormake check` names it, with that line, on a machine that lacks it. Then, with Korvid's
`kormake` on the path:

```bash
kormake
```

And to play something:

```bash
build/linux-dev/bin/blackwing play a.flac b.flac
```

[scripts/stress-play.sh](scripts/stress-play.sh) is slice 1's disk check: it plays a
directory of FLAC files through the default device while other processes hammer the
disk they are on, and reports the underruns.

```bash
scripts/stress-play.sh /media/usb/album --seconds 90
```
