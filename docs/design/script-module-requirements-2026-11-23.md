# Script modules — requirements for the spec

Nov 23, 2026. Source: design discussion. Captures what the script-module spec has to
settle before it is written. This is not the spec: no names, fields or signatures are
fixed here.

"Decided" = settled direction; everything else is a requirement the spec must meet or a
question it must answer.

This is a design record: it is kept as it stood on its date. What is being built and in
what order is in [the master PRD](../PRDs/Blackwing-Master-PRD.md) (B33, script modules
loaded at runtime, P1).

## Why this needs a spec

A script module's environment is an API contract. Once someone else's script runs against
it, every name in it has a compatibility cost. Scripts should be able to do what a native
module can, up to producing audio of their own.

Script modules are not only the phone's form of a module. A script module runs unchanged
on every platform Blackwing does, with no build per OS, which is an advantage on the
desktop too.

## Decided

| question | settled as |
|---|---|
| Shape | A projection of what Blackwing and `hifi` look like from the inside. Nothing is script-only |
| Trust | Hostile by default: wide reads, narrow writes, no exits |
| Versions | Semantic versioning, a version per surface, ranges in the manifest |
| Isolation | Desktop: out of process, in the sandbox native guests use. Phone: an in-process Lua state |
| Network | None in P1 |
| Script DSP | Best effort until there are numbers that say different |
| Lifecycle and failure | The same as native modules |

## Shape

- The script surface is a projection of the declarations native code uses: events,
  ktables, resources, the decoder contract, the stage contract. The binding is generated
  from those declarations or checked against them, so it cannot drift.
- Mirror the contracts, not the implementation. What a script sees is what crosses a
  boundary already, under the PRD's rule that no pointers cross.
- Two surfaces, named separately:
  - **The bus surface**: the `player.*`, `library.*` and `hifi.*` tables, resources and
    events. It is a contract today and can be exposed as it is.
  - **The production surface**: source, decoder and stage. This is what a script needs to
    produce or change audio. Today it is C++ structs with one caller, below the level a
    script reaches, so the spec raises it deliberately.
- A script module publishes as well as consumes: it declares its own namespaced tables
  and events, with schemas, for faces and other scripts to read.
- A script can enumerate at runtime what tables, events, capabilities and versions it has
  been given.

## Trust

Decided: hostile by default. The rule is **wide reads, narrow writes, no exits**. A script
with no way to send data out can read every table harmlessly, so the read side can
reflect the inside generously. The line is held on writes and on exits.

- **Capabilities.** A module starts with its own state and nothing else. Everything more
  is a capability its manifest asks for and the user grants. Capability names are stable
  and can be explained to a user in one line.
- **Commands are classified.** A command that makes the host do I/O or a persistent
  change on the script's behalf (adding a library root, tag write-back, anything taking a
  path or a URL) needs a capability. Transport and parameter commands can be granted
  freely.
- **Assets by handle.** The player opens the file and passes a read-only handle, as for
  native guests. A script never names a path.
- **Every exit is listed and shown closed**: the network, paths, URLs, the clipboard,
  anything a face might open from text a script published.
- **The manifest is data.** Today's `module.lua` is executable Lua. A hostile manifest is
  parsed, or evaluated in an empty capped state, before anything is granted.
- **The Lua state.** One per module. No bytecode loading, no `debug`, `io`, `os` or FFI;
  `require` reaches the module's own files only.
- **The binding is the attack surface.** Every bound function validates its arguments,
  and the binding is fuzzed.
- **Caps.** Memory through the allocator, an instruction-count hook for runaways, a time
  budget per slice, a rate limit on emitted events, row quotas on published tables.
- **Published data is hostile.** Rows and events are checked against the declared schema;
  strings are bounded and validated. Faces treat script-published text as untrusted.
- **Claims are verified, not trusted.** A stage that says it passes audio unchanged is
  not called, so the bit-perfect indicator cannot be lied to.
- **Grants have an identity.** A grant belongs to a module version. An update that widens
  its capabilities asks again.

## Versions

Decided: semantic versioning. Semver needs a declared public API, so the machine-readable
declarations are the definition of what a version number covers. No module header carries
a version number today; adding them is a prerequisite.

- **A version per surface**: the player's bus surface, the library's, `hifi`'s, the
  decoder contract, the stage contract, and the script runtime itself.
- **What a bump means, for data as well as calls**:
  - Major: removing or renaming a table, column, event or field; changing a type, a unit
    or a meaning; tightening what a command accepts; making an existing command need a
    capability.
  - Minor: adding a table, column, event, optional field, enum value, command or
    capability.
  - Patch: a fix that changes no declared shape or behaviour.
- **Behaviour counts.** Timing and ordering guarantees are part of the API: time is the
  DAC's, event order within a type, which thread and deadline a callback gets. Changing
  one is a major bump.
- **Scripts tolerate additions.** A script ignores columns, fields and enum values it
  does not know. The binding makes the tolerant path the easy one.
- **Manifest ranges.** A module declares a range per surface it uses. The host loads
  inside the range and refuses outside the major. It does not guess.
- **`0.x` is unstable.** The bus surface can reach `1.0` when P0 is done. The decoder and
  stage contracts stay `0.x` until native guests have exercised them in P1.
- **Probing** answers "is this minor addition here". It does not stand in for the
  declared range.

Accepted cost: after `1.0`, a rename inside `blackwing_player` that reaches the bus
surface breaks every face and script, so it is no longer free.

## Isolation

Decided: out of process on the desktop, in process on the phone.

- On the desktop a script module's Lua state runs in a guest process, in the same sandbox
  as native guests. A binding bug costs a guest, not the player.
- On the phone the Lua state is the sandbox, because there is no second process.
- One binding, two hosts, identical behaviour. The conformance suite passes against both.
- The phone cannot lean on what a process gives for free: unload is closing the Lua
  state, and a hang is caught by the instruction hook.
- A script sees a consistent snapshot of the tables per slice, on both hosts, never live
  state.

## Threads and time

- No script on the output callback (B4).
- A script that produces audio runs ahead, in the lead, on its own state, and feeds
  rings, as a native guest does.
- A script never runs on the tick uninterrupted: its own thread, or budgeted slices. A
  hostile script cannot stall the display.
- Every entry point states its thread, its deadline, its block size and what it may call.

## Script DSP

Decided: best effort until there are numbers that say different. iOS allows no JIT, so
the phone runs the interpreter.

- The spec promises the contract and the failure behaviour, not throughput.
- Audio crosses as block-level buffer objects, with host-side vector primitives; never a
  table per sample. This keeps a stronger promise possible later.
- An allocation and garbage-collection budget is stated per entry point.
- The harness reports cost per stage on the interpreter. Those are the numbers that would
  change this decision.

## Lifecycle and failure

Decided on Nov 22: identical for script and native.

- Load, snapshot, replace, discard. State is plain data that can be snapshotted, so a hot
  reload re-binds instead of resetting.
- Generation-stamped handles; stale events are dropped.
- Output is sanitised as a native guest's is: NaN and Inf to zero, clamp, the
  full-scale guard.
- A module that misses its deadline is bypassed with a short crossfade, and the user is
  told which one.
- A script error has a defined outcome and is reported with the module's name.

## Determinism

- A script stage declares whether it passes audio unchanged, like any stage, and states
  its effect on exact mode.
- Scripts get Korvid's stable transcendentals, so one module gives the same result on
  every platform. Korvid's Lua build already makes `math.sin` and its kin the engine's.
- The spec states Lua's number semantics against the pipeline's float32.

## The document itself

- Declarations are machine-readable: the binding, the reference pages and editor stubs
  come from one source.
- Every entry carries the same fields: thread, deadline, capability, since-version.
- A conformance suite runs the same tests against the script binding and the native
  contract.
- A headless harness lets an author test a module, with the exact-mode null as the
  oracle.
- A deprecation policy.
- An example per entry point.

## Not expressible in P1

- Anything that fetches: lyrics, cover art, online tag lookups. A host-mediated fetch can
  come later.

## Noted, not blocking

- Apple's App Store rules on downloaded interpreted code may limit script modules loaded
  at runtime on iOS. To be argued when it comes to that.
