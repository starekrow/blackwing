# dbus

A client of the D-Bus session and system buses, written from [the D-Bus
Specification](https://dbus.freedesktop.org/doc/dbus-specification.html) with no library
under it. Blackwing's Linux pieces that talk to the desktop do it through this:
`media_platform`'s MPRIS, keep-awake and sleep, and `hifi_platform`'s device reservation
for output alone. Linux's: on Windows and macOS `dbus_open` says so, and the wire format
still builds and is tested.

## What it provides

**The wire format** (`dbus.h`). `DbusWriter` writes a body to the signature it was begun
with, checking each value against it as it goes; `DbusReader` reads one, checking type,
length, alignment and nesting before it touches a byte. Every type, both byte orders
(little-endian out), the spec's limits (a 255-byte signature, 32 arrays and 32 structs
deep, 64 in all with variants, 64 MiB arrays), and ours: a message is at most 4 MiB.
`dbus_copy_value` moves a value from a reader to a writer, re-aligned for where it lands.
Names (`dbus_valid_path`, `_interface`, `_member`, `_bus_name`) and signatures
(`dbus_valid_signature`) are checked by the spec's rules.

**Messages**. `dbus_encode` makes a message's bytes from a `DbusHeader` and a body;
`dbus_parse` checks one whole (the fixed header, every header field and its type, the
fields each kind needs, the padding, the body against its signature, the descriptors it
says it has) and copies it into a `DbusMessage`, which owns its bytes and descriptors
until `dbus_message_free`. A message refused says why, in words.

**A connection**. `dbus_open(Session or System)` reads the bus's address (only `unix:`
with `path=` or `abstract=`), connects, authenticates with `EXTERNAL`, asks for
descriptors with `NEGOTIATE_UNIX_FD`, says `Hello`, and starts the connection's thread.
`dbus_open_socket` does the same over a socket already connected, which is how the tests
play a bus. Then:

- `dbus_send` posts a message from any thread and returns its serial; `dbus_call` sends a
  call and waits for its answer; `dbus_next` takes what arrived (calls to our objects,
  signals our matches asked for, replies to `dbus_send`s).
- `dbus_reply`, `dbus_reply_error`, `dbus_signal`.
- `dbus_request_name`, `dbus_release_name`, `dbus_add_match`, `dbus_remove_match`: the
  bus's own calls, waited on.
- `dbus_set_notify`: a function called on the connection's thread when `dbus_next` has
  something, and when the bus goes. It must only wake a loop.

**Objects of our own**. `dbus_export` puts an object at a path with interfaces described
as `DbusInterfaceDesc` (methods with their signatures, properties with their signature,
access and how they are signalled, signals). The connection's thread answers from that
description and from what `dbus_property_set` stored: `Introspectable.Introspect` (with
the child nodes leading down to each object), `Peer.Ping` and `GetMachineId`,
`Properties.Get` and `GetAll`. It refuses an unknown object, interface, method or
property, a call with the wrong signature, and a write to a read-only property, each
with the spec's error name. What is left comes to `dbus_next`: a described method,
`Properties.Set`, and the `Get` of a property marked `live` (a position, which changes
continuously), whose `GetAll` the caller answers with `dbus_reply_get_all`.
`dbus_property_set` sends `PropertiesChanged` when a value changes.

**The thread**. One per connection, which owns the socket. It sleeps in `net_wait` on the
socket and a waker (KR49), so an idle bus costs it no wakeups; callers post under a lock
and wake it. Nothing of a caller's runs on it but the notify function.

**Hostile input**. Everything on a bus came from another process. A message whose framing
is sound but whose content is not (a bad name, a string that is not UTF-8, a boolean of
2, a field of the wrong type, a body longer than its signature, more descriptors than it
said) is refused, counted in `dbus_refused` with its reason, and the connection kept. One
past 4 MiB is read past and refused. Bytes that cannot be framed at all end the
connection, with the reason in `dbus_lost_why`, since nothing after them can be found.

## What it is not

A bus, a code generator, a binding of any interface, or anything on Windows or macOS.
Not taken: transports other than `unix:` (`tcp:`, `unixexec:`, `launchd:`), the
`ANONYMOUS` and `DBUS_COOKIE_SHA1` mechanisms, a server's side of the authentication,
and message types past the four. Each is refused in words.

## Tests

`kormake test dbus`. The wire format against bytes worked out by hand from the spec (an
empty `ax` padded to 8, an array's length not counting the padding before its first
element), the names at their edges, a big-endian peer's message made by hand. Fuzzing,
two ways: random signatures with random values, written, encoded, parsed and copied back
byte for byte; and those messages mutated (bits flipped, lengths changed, ends cut, junk
added), each refused in words or read to its end. 3,000 rounds by default;
`DBUS_FUZZ_ROUNDS=1000000` runs a million (40 million mutated messages, 90 seconds under
the address sanitizer).

Then the connection three ways: a stand-in bus played over a socket pair, which sends
what a real bus would not (a bad member name, a 4 MiB message, then a good signal, then
bytes that cannot be framed); a private `dbus-daemon` started for the test, with two
connections calling, answering, refusing, getting and setting properties, matching
signals, introspecting and passing a descriptor through it, and idle for a second with no
wakeups; and the real session bus, where a name with MPRIS's identity is owned and
`busctl` reads its `Identity`. The last two are skipped, with a line, where there is no
`dbus-daemon` or no session bus.

## See also

- [The slice 5 PRD](../../docs/PRDs/Blackwing-Slice-5-PRD.md), "D-Bus": why it is ours.
- [Slice 5's notes](../../docs/design/slice-5-notes.md): what building it found, with the numbers.
- [modules](../README.md): the other modules and how they depend on each other.
