# Blackwing

A hi-fi local music player built on the [Korvid](../korvid) engine. It plays FLAC files
from the command line into a capture device; a real output device is next.

- [docs/README.md](docs/README.md): where the documents are.
- [docs/PRDs/Blackwing-Master-PRD.md](docs/PRDs/Blackwing-Master-PRD.md): what is being
  built, in what order.
- [modules/README.md](modules/README.md): the modules.

## Building

Korvid is expected beside this checkout, at `../korvid` ([korvid.config](korvid.config)
says so). With Korvid's `kormake` on the path:

```bash
kormake
```
