// Blackwing dbus: a client of the D-Bus session and system buses, with no library under it.
//
//   char why[160];
//   DbusConnection* c = dbus_open(DbusBus::Session, 2.0, why, sizeof why);
//   if (!c) ... why
//   dbus_request_name(c, "org.mpris.MediaPlayer2.blackwing", kDbusNameDoNotQueue, 2.0);
//   dbus_export(c, "/org/mpris/MediaPlayer2", interfaces, n);  // methods, properties, signals
//   DbusWriter v; v.begin("s"); v.str("Playing");
//   dbus_property_set(c, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", "PlaybackStatus", v);
//   dbus_set_notify(c, wake_my_loop, me);                      // on the connection's thread: only wake
//   ... in a tick:
//   DbusMessage m;
//   while (dbus_next(c, &m)) {                                 // calls to our objects, signals
//       DbusReader r = m.reader(); ... r.str(&s) ...
//       dbus_reply(c, m, nullptr);
//       dbus_message_free(&m);
//   }
//   dbus_close(c);
//
// The connection owns its socket on a thread of its own, asleep until the bus speaks or a
// caller posts: an idle bus costs no wakeups. Nothing of a caller's runs on that thread but
// the notify function. Every function here may be called from any thread; dbus_next and
// the blocking calls (dbus_call, dbus_request_name, dbus_add_match) from one thread at a time
// each. Everything that arrives is checked before it is read: a message that is not what
// the specification says is refused in words (dbus_refused) and the connection kept.
//
// What it is not: a bus, a code generator, or anything on Windows or macOS, where
// dbus_open says so. modules/dbus/README.md has the rest.
#pragma once
#include "korvid/core/array.h"
#include "korvid/core/handle.h"
#include "korvid/core/types.h"

namespace kv {

// ---- the specification's limits, and ours ----------------------------------------------
constexpr u32 kDbusSpecMaxMessage = 1u << 27; // 128 MiB, the spec's
constexpr u32 kDbusMaxMessage     = 1u << 22; // 4 MiB, ours: a longer message is read past and refused
constexpr u32 kDbusMaxArray       = 1u << 26; // 64 MiB, the spec's
constexpr u32 kDbusMaxSignature   = 255;
constexpr u32 kDbusMaxNesting     = 32;       // arrays, and structs, each; 64 with variants, in all
constexpr u32 kDbusMaxDepth       = 64;
constexpr u32 kDbusMaxName        = 255;      // a bus, interface, member or error name
constexpr u32 kDbusMaxFds         = 16;       // descriptors one message carries, ours

// ---- names ----------------------------------------------------------------------------
bool dbus_valid_path(const char* s);       // "/", or "/a/b_2": no empty element, no trailing '/'
bool dbus_valid_interface(const char* s);  // two or more elements of [A-Za-z_][A-Za-z0-9_]*; an error name too
bool dbus_valid_member(const char* s);     // [A-Za-z_][A-Za-z0-9_]*
bool dbus_valid_bus_name(const char* s);   // ":1.42", or "org.mpris.MediaPlayer2.blackwing"
// A signature: complete types only, nesting within the limits, dict entries only as an
// array's elements with a basic key. `single`: exactly one complete type (a variant's).
bool dbus_valid_signature(const char* s, bool single = false);

// ---- writing values -------------------------------------------------------------------
// A body, written to the signature it is declared to be: each value is checked against it
// as it goes, and the first that does not fit is kept in `error` and the rest ignored.
//
//   DbusWriter w; w.begin("sa{sv}");
//   w.str("org.mpris.MediaPlayer2.Player");
//   w.array_begin(); w.entry_begin(); w.str("Volume"); w.variant_begin("d"); w.f64(0.5); w.variant_end(); w.entry_end(); w.array_end();
//   if (!w.done()) ... w.error
struct DbusWriter {
    struct Frame {
        kv::u8  code = 0;        // 0 the top, 'a', '(', '{', 'v'
        kv::u8  in_bytes = 0;    // the frame's signature is in `bytes` (a variant's), not `sig`
        kv::u32 at = 0, end = 0; // the signature still to write, as offsets
        kv::u32 elem_at = 0;     // an array's element signature starts here
        kv::u32 len_at = 0;      // an array's length, to fill at its end
        kv::u32 start = 0;       // an array's first element
        kv::u32 count = 0;       // an array's elements so far
    };
    Array<u8>   bytes;            // the body so far; aligned as if it starts a message's body
    char        sig[kDbusMaxSignature + 1] = {};
    kv::u32         sig_len = 0;
    Frame       stack[kDbusMaxDepth + 1];
    kv::u32         depth = 0;
    Handle      fds[kDbusMaxFds]; // what fd() was given; sent with the message, kept by the caller
    kv::u32         fd_count = 0;
    const char* error = nullptr;  // the first misuse, in words; a constant

    // Starts again, to be `signature` ("" for no body). False for a signature that is not one.
    bool begin(const char* signature);
    void byte(kv::u8 v);
    void boolean(bool v);
    void i16(kv::i16 v);
    void u16(kv::u16 v);
    void i32(kv::i32 v);
    void u32(kv::u32 v);
    void i64(kv::i64 v);
    void u64(kv::u64 v);
    void f64(kv::f64 v);
    void str(const char* s);                   // valid UTF-8 with no NUL, else refused
    void str_n(const char* s, kv::u32 len);
    void path(const char* s);
    void signature(const char* s);
    void fd(Handle h);                         // the handle's index in `fds`
    void array_begin();
    void array_end();
    void struct_begin();
    void struct_end();
    void entry_begin();                        // a dict entry: only as an array's element
    void entry_end();
    void variant_begin(const char* signature); // one complete type
    void variant_end();
    // Every value the signature asked for written, every container closed, nothing refused.
    bool done() const;
    void free();
};

// ---- reading values -------------------------------------------------------------------
// A body read against its signature. Every read checks the type, the length and the
// alignment before it touches a byte; the first that fails sets `failed` (and `error`) and
// every read after says false. Strings are NUL-terminated pointers into the message.
//
//   DbusReader r = m.reader();
//   const char* name; r.str(&name);
//   if (r.array_begin()) while (r.array_more()) { r.entry_begin(); ...; r.entry_end(); }
struct DbusReader {
    struct Frame {
        kv::u8          code = 0;
        const char* at = nullptr;      // the signature still to read
        const char* end = nullptr;
        const char* elem_at = nullptr; // an array's element signature
        kv::u32         data_end = 0;      // where an array's (or the top's) bytes end
        bool        started = false;   // an array: an element has begun
    };
    const kv::u8*     data = nullptr; // aligned as a message is: offset 0 is 8-aligned
    kv::u32           pos = 0, limit = 0;
    bool          swap = false;   // the message's byte order is not this machine's
    kv::u32           fd_count = 0;
    Frame         stack[kDbusMaxDepth + 1];
    kv::u32           depth = 0;
    kv::u32           arrays = 0, structs = 0;
    bool          failed = false;
    const char*   error = nullptr;

    // Over `len` bytes of `data` from `pos`, to be `signature`.
    void begin(const kv::u8* data, kv::u32 len, const char* signature, bool swap = false, kv::u32 pos = 0, kv::u32 fd_count = 0);
    // The next value's type code: 'i', 's', 'a', '(', '{', 'v'...; 0 at the end of what is
    // being read (a struct's, a variant's, an array's element, the body).
    char peek() const;
    bool byte(kv::u8* v);
    bool boolean(bool* v);
    bool i16(kv::i16* v);
    bool u16(kv::u16* v);
    bool i32(kv::i32* v);
    bool u32(kv::u32* v);
    bool i64(kv::i64* v);
    bool u64(kv::u64* v);
    bool f64(kv::f64* v);
    bool str(const char** s, kv::u32* len = nullptr);
    bool path(const char** s);
    bool signature(const char** s);
    bool fd(kv::u32* index);           // into the message's fds
    bool array_begin();
    bool array_more();                 // true while there is another element; at the end, closes the array
    bool struct_begin();
    bool struct_end();
    bool entry_begin();
    bool entry_end();
    bool variant_begin(const char** signature = nullptr);
    bool variant_end();
    bool skip();                       // one complete value, whatever it is
    bool at_end() const;               // the body (or the container) read to its end
    // Any number, as a variant of a number type holds it: 'y', 'n', 'q', 'i', 'u', 'x', 't', 'd'.
    bool number(kv::f64* v);
};

// Copies one complete value from `r` to `w`, re-aligned for where it lands: a property's
// value into a reply, a variant out of one message into another.
bool dbus_copy_value(DbusReader& r, DbusWriter& w);

// ---- messages -------------------------------------------------------------------------
enum class DbusKind : u8 { Invalid = 0, Call = 1, Return = 2, Error = 3, Signal = 4 };
constexpr u8 kDbusNoReplyExpected = 0x1;
constexpr u8 kDbusNoAutoStart     = 0x2;
constexpr u8 kDbusInteractive     = 0x4;

// What a message to send is, but its body. Pointers are read during the call only.
struct DbusHeader {
    DbusKind    kind = DbusKind::Call;
    u8          flags = 0;
    const char* destination = nullptr;
    const char* path = nullptr;
    const char* interface = nullptr;
    const char* member = nullptr;
    const char* error_name = nullptr;
    u32         reply_serial = 0;
};

// A message that arrived (or was parsed), owning a copy of its bytes and its descriptors.
// Plain data: copied about by value, freed once with dbus_message_free.
struct DbusMessage {
    DbusKind    kind = DbusKind::Invalid;
    u8          flags = 0;
    bool        big_endian = false;
    u32         serial = 0;
    u32         reply_serial = 0;
    const char* path = "";        // "" where the header has none
    const char* interface = "";
    const char* member = "";
    const char* error_name = "";
    const char* destination = "";
    const char* sender = "";
    const char* signature = "";   // the body's
    const u8*   body = nullptr;
    u32         body_len = 0;
    Handle      fds[kDbusMaxFds]; // the caller's once taken (dbus_message_take_fd); else closed by free
    u32         fd_count = 0;
    u8*         storage = nullptr;
    u32         storage_len = 0;

    DbusReader reader() const;
    // An error's text: its body's first string, when it has one; "" if not.
    const char* error_text() const;
};

// Bytes the message starting at `bytes` takes in all, once its first 16 are in: 0 while
// fewer have come, -1 when its framing cannot be a message (*why: a constant).
i64  dbus_message_size(const u8* bytes, u32 have, const char** why);
// Parses one whole message, checking every part, and copies it. `fds` are the descriptors
// that came with it; the message holds them whether it parses or not (closed by free).
// False, *why in words.
bool dbus_parse(const u8* bytes, u32 len, const Handle* fds, u32 fd_count, DbusMessage* out, const char** why);
// A message's bytes, little-endian, at `serial`. False, *why, for a header that is not one
// (a call with no path or member, a name that is not valid) or a body not done.
bool dbus_encode(const DbusHeader& h, u32 serial, const DbusWriter* body, Array<u8>* out, const char** why);
// Takes descriptor i out of the message: the caller's to close (net_close) from now.
Handle dbus_message_take_fd(DbusMessage* m, u32 i);
void   dbus_message_free(DbusMessage* m);

// ---- addresses ------------------------------------------------------------------------
// One place a bus listens, from an address's ';'-separated list. Only `unix:` with `path=`
// or `abstract=` is taken; the rest are refused in words.
struct DbusAddress {
    char path[108] = {};
    bool abstract = false;
};
// The first usable entry of `address` (%-escapes undone). False, *why, when none is.
bool dbus_parse_address(const char* address, DbusAddress* out, u32 cap, u32* count, const char** why);

// ---- a connection ---------------------------------------------------------------------
enum class DbusBus : u8 { Session, System };
struct DbusConnection;

// Connects (DBUS_SESSION_BUS_ADDRESS, else $XDG_RUNTIME_DIR/bus; DBUS_SYSTEM_BUS_ADDRESS,
// else /var/run/dbus/system_bus_socket), authenticates as this user with EXTERNAL, asks for
// descriptors, says Hello, and starts the connection's thread. Null, `why` in words.
DbusConnection* dbus_open(DbusBus bus, f64 seconds, char* why, u32 why_cap);
DbusConnection* dbus_open_address(const char* address, f64 seconds, char* why, u32 why_cap);
// The same over a socket already connected to a bus (or to a test's stand-in for one).
// Takes the socket's handle; `s` is left closed.
struct NetSocket;
DbusConnection* dbus_open_socket(NetSocket* s, f64 seconds, char* why, u32 why_cap);
// Stops the thread, closes the socket, frees what is queued. No notify after it returns.
void dbus_close(DbusConnection* c);

const char* dbus_unique_name(DbusConnection* c); // ":1.42", from Hello
bool        dbus_can_pass_fds(DbusConnection* c);
bool        dbus_is_open(DbusConnection* c);     // false once the bus has gone
const char* dbus_lost_why(DbusConnection* c);    // why, in words, once it has
// Messages refused since the connection opened (not what the spec says, or past our size),
// and the last one's reason in `why` (may be null).
u32         dbus_refused(DbusConnection* c, char* why, u32 why_cap);

// How many times the connection's thread has woken from its wait: for a test that an idle
// bus costs nothing.
u64         dbus_thread_wakes(DbusConnection* c);

// Called on the connection's thread when dbus_next has something new, and once when the
// bus goes. It must only wake its loop (platform_wake): never call back into the connection.
void dbus_set_notify(DbusConnection* c, void (*fn)(void* user), void* user);

// Sends a message: its serial, or 0 when it could not be (the connection gone, a header or
// body not valid: logged). Any thread; returns at once.
u32  dbus_send(DbusConnection* c, const DbusHeader& h, const DbusWriter* body);
// What arrived for the caller, oldest first: calls to its objects, signals its matches
// asked for, replies to calls it sent with dbus_send. False when there is nothing.
bool dbus_next(DbusConnection* c, DbusMessage* out);
// Sends a call and waits up to `seconds` for its answer: true with the reply (a Return or
// an Error: check kind) in *reply, which the caller frees; false, `why`, when the time ran
// out or the connection went.
bool dbus_call(DbusConnection* c, const DbusHeader& h, const DbusWriter* body, f64 seconds, DbusMessage* reply,
               char* why = nullptr, u32 why_cap = 0);
// Answers a call that arrived, unless it said it wants no reply.
bool dbus_reply(DbusConnection* c, const DbusMessage& call, const DbusWriter* body);
bool dbus_reply_error(DbusConnection* c, const DbusMessage& call, const char* error_name, const char* text);
u32  dbus_signal(DbusConnection* c, const char* path, const char* interface, const char* member, const DbusWriter* body);

// ---- names and matches (the bus's own calls, waited on) -------------------------------
constexpr u32 kDbusNameAllowReplacement = 0x1;
constexpr u32 kDbusNameReplaceExisting  = 0x2;
constexpr u32 kDbusNameDoNotQueue       = 0x4;
enum class DbusNameReply : u8 { Failed = 0, PrimaryOwner = 1, InQueue = 2, Exists = 3, AlreadyOwner = 4 };
DbusNameReply dbus_request_name(DbusConnection* c, const char* name, u32 flags, f64 seconds);
bool          dbus_release_name(DbusConnection* c, const char* name, f64 seconds);
// "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'"
bool          dbus_add_match(DbusConnection* c, const char* rule, f64 seconds);
bool          dbus_remove_match(DbusConnection* c, const char* rule, f64 seconds);

// ---- objects of our own ---------------------------------------------------------------
// An interface, described: the connection answers Introspect, Peer, and Properties.Get and
// GetAll from what is described and set here, on its own thread; it refuses a call to a
// method not described, or with the wrong signature, in words. The rest (a described
// method, Properties.Set, a live property's Get) comes to dbus_next for the caller to answer.
// Descriptions are the caller's, and must outlive the connection.
enum class DbusAccess : u8 { Read, Write, ReadWrite };
enum class DbusEmits : u8 { Changed, Invalidates, None }; // what PropertiesChanged says of it
struct DbusArg       { const char* name; const char* signature; };
struct DbusMethodDesc {
    const char*    name;
    const char*    in = "";            // the arguments' signature, all of them
    const char*    out = "";
    const DbusArg* args = nullptr;     // names for Introspect, in then out; optional
    u32            arg_count = 0;
};
struct DbusPropertyDesc {
    const char* name;
    const char* signature;
    DbusAccess  access = DbusAccess::Read;
    DbusEmits   emits = DbusEmits::Changed;
    bool        live = false; // its value is asked of the caller each time (a position): Get and GetAll go to dbus_next
};
struct DbusSignalDesc { const char* name; const char* signature = ""; const DbusArg* args = nullptr; u32 arg_count = 0; };
struct DbusInterfaceDesc {
    const char*             name;
    const DbusMethodDesc*   methods = nullptr;    u32 method_count = 0;
    const DbusPropertyDesc* properties = nullptr; u32 property_count = 0;
    const DbusSignalDesc*   signals = nullptr;    u32 signal_count = 0;
};
// An object at `path` with these interfaces (replacing any it had). False for a path not
// valid, or past 32 objects.
bool dbus_export(DbusConnection* c, const char* path, const DbusInterfaceDesc* const* interfaces, u32 count);
bool dbus_unexport(DbusConnection* c, const char* path);
// A property's value, written to its signature (a DbusWriter begun with it). Sends
// PropertiesChanged when the value is not what it was and the property emits, unless
// `quiet`. False for a property not described, or a value not its signature.
bool dbus_property_set(DbusConnection* c, const char* path, const char* interface, const char* name,
                       const DbusWriter& value, bool quiet = false);
// Several at once: one PropertiesChanged for all of them.
struct DbusPropertyValue { const char* name; const DbusWriter* value; };
bool dbus_properties_set(DbusConnection* c, const char* path, const char* interface,
                         const DbusPropertyValue* values, u32 count, bool quiet = false);
// Answers a Properties.GetAll that came to dbus_next (an interface with a live property):
// what is set, with the live ones' values given here.
bool dbus_reply_get_all(DbusConnection* c, const DbusMessage& call, const DbusPropertyValue* live, u32 count);

} // namespace kv
