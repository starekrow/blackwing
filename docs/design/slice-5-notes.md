# Slice 5 notes

What building [slice 5](../PRDs/Blackwing-Slice-5-PRD.md) found, milestone by milestone,
with the numbers. Linux and `linux-dev` unless said.

## Before milestone 0

Nov 27, 2026.

- **Blackwing did not build with Korvid's kormake.** Korvid's `bff599d` added
  `test_files`, checked against the project's root rather than the engine's
  (`tools/kormake/scripts/emit.lua`), so a project linking `krome` failed before
  compiling. Fixed in Korvid on the branch `kormake-test-files-root` (`3b7221e`), not
  yet merged there. Blackwing needs that branch, or Korvid's `main` once it has it.
- **Nothing else in Korvid's slice 13 or Krome's 7 and 8 broke Blackwing.** Every suite
  passed on the fixed kormake before any change of this slice's.

## Milestone 0, the catch-up

Nov 27, 2026.

**KR10, the render thread waits.** Its naps were sleeps of 2 to 50 ms for want of a
timed wait. Each is now `os_cond_timed_wait` on the thread's own condition, ended at once
by a command, `hifi_play`, or the new `hifi_wake`, which the player calls when a piece of
a file `hifi` holds comes in. A wake sent before the wait began is kept in a flag. Playing
out, the thread now looks every 20 ms rather than every 2. Clean under the thread
sanitizer. Two short waits remain as sleeps: for the callback to take up a join, which the
realtime thread cannot signal, half a millisecond at a time and a fade's length at most.
Not measured yet: slice 2's 2 to 13 ms for a control through PipeWire, which this should
move down by up to the old nap (5 ms with a full lead). Milestone 1 plays through PipeWire
and measures it again.

**KR15 and KR16.** The corpus's 1,100-byte path is made in one `io_make_dirs`; the
watcher's link filter is gone, and the corpus's loop (a link back up to the root) still
costs nothing. On Apple `io_make_dirs` stops at the OS's 1,024 bytes, so the corpus test's
long path will want a shorter form there (milestone 6).

**The index in the cache directory** (KR24): `~/.cache/blackwing/blackwing/library/`
on Linux; the roots and overrides stay in `~/.local/share/blackwing/blackwing/library/`.
An index found in the old place is moved once, with its WAL. Tried by hand with the XDG
directories pointed at a scratch tree: moved, and opened.

**An override file set aside is published**: `LibraryOverridesSetAside` and
`library.scan`'s `set_aside`, on `ui_dev`'s Scan panel. The conformance walk now leaves a
broken override file for the root it adds, so it hears the event with its fields.

**The window's place** (KR42). Kept and put back where the platform lets a program place
a window: under X11 (XWayland on GNOME), put back at 300, 200 as kept. Places off every
screen were clamped by Mutter before Blackwing's own check was needed (50000, 40000 landed
at 0, 138; -1250, 300 at 0, 300); Windows does not clamp, and is where the check will be
seen to work. Under Wayland nothing is placed and the place last kept is left alone.

| suite | checks |
|---|---|
| `hifi` | 342 |
| `blackwing_player` | 195 |
| `blackwing_library` | 219 (215 before: the long path checked, the set-aside event and count) |
| `blackwing_script` | 41 |
| `ui_dev` | 284 |
| the rest | as before |
