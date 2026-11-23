# media_platform

The operating system's media surface: media keys and remote commands come in as the same
events a face emits, and what is playing, its art and its position go out. It also keeps
the system awake during playback and reports interruptions.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- Backends: MPRIS on Linux, SMTC on Windows, the Now Playing and remote command centres
  on macOS.
- Keep-awake while playing.
- Interruptions and resumes, as events.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
