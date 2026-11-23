# hifi_platform

The audio output device, per OS, with the control a hi-fi player needs: which device, at
what rate and depth, shared or exclusive, how late it says it is, and what to do when it
is unplugged. `hifi` calls it; players and faces do not.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- Enumeration, capabilities, and device events (added, removed, default changed).
- Opening at a format; rate switching; exclusive mode where the OS has it.
- The callback on the OS's realtime audio thread class.
- Reported latency and presentation time.
- Backends: PipeWire on Linux first; WASAPI on Windows and Core Audio on macOS in P0.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
