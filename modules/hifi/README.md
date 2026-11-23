# hifi

The hard-realtime audio path and the interfaces around it: audio flows from sources
through stages into the lead and out through the device's callback, and nothing on that
path waits, allocates or locks. It knows nothing of tracks, queues, tags or players. It
renders what it is handed and says what it did; `blackwing_player` decides what that is.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- The flow: sources, stages, the lead, the rings, the output callback.
- The interfaces: what a source is (a decoder behind `hifi_codec`'s contract), what a
  stage is (`hifi_eq` is one), what the output is (`hifi_platform`).
- The recut's mechanism: a second render near the playhead, and the join that crossfades
  into it.
- Joining one source to the next in one output stream.
- Format conversion to the device: resampling, dither.
- The clock: position at the DAC, from the device's reported latency.
- What it publishes: `hifi.output`, `hifi.devices`, `hifi.chain`, `hifi.meter`, and the
  bit-perfect state computed from what the stages declare.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
