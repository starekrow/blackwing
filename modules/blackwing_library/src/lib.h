// What the library's threads share. The main thread (the module's system, every tick)
// takes commands and publishes; the scan thread walks; the writer keeps the index; the
// reader answers windows. They speak through inboxes under one mutex, each with its own
// condition variable, and nobody holds the mutex while working.
#pragma once
#include "index.h"
#include "korvid/core/array.h"
#include "korvid/core/os_thread.h"

namespace kv::lib {

constexpr u32 kPathMax = 4096;

// Strings in one growing block, named by offset, so records can be kept in arrays.
struct Strings {
    Array<char> bytes;
    u32 add(const char* s, u32 n) {
        u32 at = bytes.len;
        char* p = bytes.push_n(n + 1);
        memcpy(p, s, n);
        p[n] = 0;
        return at;
    }
    u32 add(const char* s) { return add(s, (u32)strlen(s)); }
    const char* at(u32 off) const { return bytes.data + off; }
    void clear() { bytes.clear(); }
    void free() { bytes.free(); }
};

// What the walk found, on its way to the writer: offsets into `s`.
struct DirMsg   { u32 root; u32 path, parent, name, reason; u8 state; };
struct TrackMsg { u32 root; u32 dir, path, name, shown, title, artist, album; u64 size; i64 mtime; u32 track, disc; };
struct Batch {
    Strings         s;
    Array<DirMsg>   dirs;
    Array<TrackMsg> tracks;
    void free() { s.free(); dirs.free(); tracks.free(); }
};

// The tags pass. The writer says which tracks want reading (new, or changed since they
// were read); the tagger reads them through the main thread's I/O and says what it found.
struct TagJob { u64 track; u32 root; u32 rel; u64 size; i64 mtime; };
struct TagJobs {
    Strings       s;
    Array<TagJob> jobs;
    u32           next = 0;   // the tagger's place in it
    void free() { s.free(); jobs.free(); }
};
// What one track's read found: its values (every source's), the merge, its audio.
struct TagVal { u8 field, source; u32 text; };
struct TagRec {
    u64 track = 0; u32 root = 0;
    u64 size = 0; i64 mtime = 0;          // what was read: the names pass's size and time
    u32 rate = 0; u16 channels = 0, bits = 0; u64 frames = 0; u8 estimated = 0;
    u32 reason = 0;                        // offset of "" when it all went well
    u32 vals = 0, nvals = 0;               // into the batch's values
    u32 title = 0, artist = 0, album = 0, albumartist = 0, date = 0, genre = 0, shown = 0;
    u32 trackno = 0, discno = 0;
};
struct TagBatch {
    Strings       s;
    Array<TagVal> vals;
    Array<TagRec> recs;
    void free() { s.free(); vals.free(); recs.free(); }
};

// The slot: what someone is looking at, as the writer is told it to resolve.
enum class SlotKind : u8 { None, Path, Dir, Tracks };
struct SlotMsg {
    SlotKind kind = SlotKind::None;
    u64      dir = 0;
    char     path[kPathMax] = "";       // absolute, for Path
    Array<u64> tracks;                  // for Tracks (a window's rows too)
};

// For the writer, in the order sent.
enum class WriteKind : u8 { AddRoot, RemoveRoot, Batch, Walked, SaveRoots, Tags, Slot };
struct WriteMsg {
    WriteKind kind;
    u32       root = 0;
    f64       seconds = 0.0;
    Batch*    batch = nullptr;
    TagBatch* tags = nullptr;
    SlotMsg*  slot = nullptr;
    char      path[kPathMax] = "";
};

// A read the tagger wants, and its answer.
struct ReadAsk  { u32 job = 0, gen = 0; u64 at = 0, len = 0; bool slot = false; char path[kPathMax]; };
struct ReadDone { u32 job = 0, gen = 0; u64 at = 0; void* data = nullptr; usize size = 0; i32 err = 0; bool ok = false; };

// For the main thread.
enum class NoteKind : u8 { Counts, Walked, Offline, Committed };
struct Note {
    NoteKind kind;
    u32 root = 0;
    u32 tracks = 0, dirs = 0, failed = 0, unread = 0;
    u64 generation = 0, total = 0;
    f64 seconds = 0.0;
    char reason[96] = "";
};

// A window's answer.
struct Result {
    u32 id = 0, seq = 0, total = 0, first = 0, count = 0;
    u64 generation = 0;
    LibraryRow* rows = nullptr; // count of them, from mem_alloc
};
struct Ask { LibraryQuery q; u32 seq = 0; };

// For the scan thread.
struct Walk { u32 root; char path[kPathMax]; };

struct Shared {
    OsMutex lock;
    OsCond  scan_wake, write_wake, read_wake;
    bool    quit = false;
    bool    ready = false;            // the writer has the index open: the reader may open it
    char    index_path[kPathMax] = "";
    char    roots_path[kPathMax] = "";
    // main -> scan
    Array<Walk> walks;
    Array<u32>  drops;
    // main, scan -> writer
    Array<WriteMsg*> writes;
    // main -> reader: the newest ask for each window
    Array<Ask> asks;
    // writer -> tagger: tracks to read; the slot's, which goes first, replaced whole
    Array<TagJobs*> jobs;
    TagJobs*        slot_jobs = nullptr;
    // tagger <-> main: reads through the I/O queue
    Array<ReadAsk>   read_asks;
    Array<ReadDone>  read_dones;
    OsCond           tag_wake;
    // main -> scan: a directory to list before the rest
    u32              walk_next_root = 0;
    char             walk_next[kPathMax] = "";
    // writer, reader -> main
    Array<Note>   notes;
    Array<Result> results;
    // Counters anyone may read without the lock (atomics).
    u64 generation = 0;   // the writer's commits
    u64 dirs_listed = 0, files_found = 0, tags_read = 0;
    u32 tags_active = 0;  // tracks the tagger is reading now
    u32 walking = 0;      // the root being walked
    char walking_dir[256] = ""; // under the lock
    bool rebuilt = false;
    // The roots, as the writer saves them: under the lock.
    struct RootLine { u32 id; char path[kPathMax]; };
    Array<RootLine> root_lines;
};

void scan_thread(void* shared);
void write_thread(void* shared);
void read_thread(void* shared);
void tag_thread(void* shared);

// The audio files the walk lists, by extension.
bool audio_name(const char* name);
// A name or path as UTF-8, for showing and searching: what is not becomes U+FFFD.
u32 shown_utf8(const char* s, char* out, u32 cap);

} // namespace kv::lib
