# blackwing_tags

Tags, read from every source and never written. Each source (embedded tags of each kind,
a cue sheet, the file's name and path, a folder's image) has a specificity rank; the
best-ranked value for each field wins, and the user's overrides in a sidecar outrank them
all.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- Readers: ID3v1 and v2, Vorbis comments, APE, MP4 atoms, RIFF and AIFF chunks, cue sheets.
- The ranking, and tags guessed from a path.
- Sidecar overrides: the format, reading and writing them.
- Embedded and folder art, as bytes for someone else to decode.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
