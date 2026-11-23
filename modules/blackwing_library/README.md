# blackwing_library

The library: you add a directory and that is the whole of setting it up. Files are
playable after the first pass over their names; tags, art and the browsing views fill in
behind; and whatever the user looks at moves to the next-to-scan slot.

**Status:** a skeleton. No code yet; this page says what the module will hold.

## What it will hold

- Roots, and the three passes: names, tags, background work.
- The priority slot (`LibraryPrioritize`).
- The index: local sqlite in WAL mode, a cache that can be rebuilt.
- Watching for changes, with polling for network shares; roots that go offline.
- P1: looking inside a disc image (`.iso`) as inside a directory.
- Queries: asynchronous, cancellable, paged for virtualized lists.
- Its tables and events. The PRD has the sketch.

## See also

- [The master PRD](../../docs/PRDs/Blackwing-Master-PRD.md): the plan this module is part of.
- [modules](../README.md): the other modules and how they depend on each other.
