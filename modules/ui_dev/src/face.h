// ui_dev's state, and what its files share: the model writer, which writes a value only
// when it is not what the screen already shows, and the panels' two halves (a table read
// into the model, an event turned into a command).
#pragma once
#include "korvid/blackwing_library/library.h"
#include "korvid/blackwing_session/session.h"
#include "korvid/core/engine.h"
#include "korvid/core/json.h"
#include "korvid/core/name.h"
#include "korvid/krome/krome.h"
#include "korvid/ui_dev/ui_dev.h"

namespace kv {

constexpr u32 kFaceKeys = 768;   // model keys the writer remembers
constexpr u32 kFaceParams = 192; // parameters the Chain panel has keys for

// One model key as the screen shows it: a hash of the value last written.
struct FaceKey { const char* key = nullptr; u64 shown = 0; };

// The windows the face opens onto the library, and the ids it collects under: any numbers
// nobody else is likely to pick.
constexpr u32 kFaceLibWindow   = 0xD0E00001; // the Library panel's list
constexpr u32 kFaceTitleWindow = 0xD0E00002; // the queue's rows, by their paths
constexpr u32 kFaceCollect     = 0xD0E00011; // a row's tracks, to be queued
constexpr u32 kFaceCollectPlay = 0xD0E00012; // the same, and played once they are all in
constexpr u32 kFaceView        = 512;        // rows of a list handed over at once, at most
constexpr u32 kFaceCrumbs      = 12;
// A text read back from the screen (krome_model_text) is 511 bytes at most, cut without a
// word when it is longer. One this long may have been cut, and is not taken for a path.
constexpr u32 kFaceTypedMax    = 511;

// Where the Library panel's list is: a view, what it is of, and what to call it.
struct FaceCrumb { LibraryView view = LibraryView::Folders; u64 parent = 0; char name[96] = ""; };
// A window as it was last asked for.
struct FaceAsk {
    LibraryView view = LibraryView::Count; LibrarySort sort = LibrarySort::Default; u64 parent = 0; char text[128] = ""; u32 first = 0, count = 0;
};
// A path the title window was given, at a queue row's index.
struct FaceTitle { u32 index = 0; u64 hash = 0; };
// Which of the inspectors' tabs is up, as the model has it: the others do no work.
enum class FaceTab : u8 { Chain, Scan, Events, Tables };
// An event as the Events panel keeps it: the number it was seen at, and its line.
struct FaceEvent { u64 n = 0; char text[244] = ""; };
constexpr u32 kFaceEvents = 512; // events kept

struct Face {
    Engine* e = nullptr;
    Name    screen;            // Krome's name for the shell; null while none is open
    bool    wanted   = false;  // the shell is to be opened at the next tick
    bool    ready    = false;  // built, and written once from the tables
    bool    hidden   = false;  // the window is not seen: nothing is read, written or asked
    bool    fresh    = true;   // everything is to be written at the next sync (just built, or seen again)
    u32     awake    = 0;      // ticks still to ask for, after a command: its answer is a tick or two away
    u32     attempts = 0;      // times the shell was opened this run
    u32     stubbed  = 0;      // a bit per panel standing in for a file that did not load
    FaceKey keys[kFaceKeys];
    u32     nkeys = 0;
    UiDevStats stats;

    // Transport: the bar is the pointer's while it is held; the length the bar was last
    // drawn against, which is what a place on it means.
    bool    seek_drag = false;
    u64     bar_frames = 0;
    // Queue: the rows its list has built, as it last said.
    u32     q_first = 0, q_count = 0;
    // The queue's titles: what the library was asked of the rows in view (their paths, by
    // row), and this tick's answers, by place in the view; and the transport's row.
    FaceTitle titles[kFaceView];
    u32     ntitles = 0;
    u32     titles_rest = 0;   // ticks the view has been where it is
    u32     titles_first = 0, titles_count = 0;
    const LibraryRow* title_rows[kFaceView]; // valid for the tick: row q_first + k of the queue
    const LibraryRow* title_now = nullptr;   // the transport's row
    // Library: where the list is (the last crumb), how it is sorted, what is searched for;
    // the rows its list has built; what was last asked, and what came back.
    FaceCrumb crumbs[kFaceCrumbs];
    u32     ncrumbs = 1;
    LibrarySort lib_sort = LibrarySort::Default;
    char    lib_text[128] = "";
    u32     lib_first = 0, lib_count = 0;
    FaceAsk lib_asked;
    bool    lib_open = false;      // the window is open in the library
    bool    lib_answered = false;  // what was last asked has come back
    bool    lib_stale = false;     // the rows in the table are another list's: not shown
    u64     lib_ask_tick = 0;
    u32     lib_total = 0;
    f64     lib_seconds = 0.0;
    u64     lib_generation = 0;
    f64     lib_moved_at = 0.0;    // when the list was last somewhere else
    bool    lib_prioritized = false;
    u64     edit_track = 0;        // the track whose field is being edited
    u32     play_in = 0;           // ticks until PlayerPlay: what was collected to play is in
    // The inspectors: which is up. Events: the last kFaceEvents that passed, a ring from
    // the general allocator; whether they are being kept (the tab is up, the window seen,
    // not held). Tables: which name is picked, and the lines its list has built.
    FaceTab tab = FaceTab::Chain;
    FaceEvent* events = nullptr;
    u32     ev_head = 0, ev_count = 0;
    u64     ev_seen = 0;
    bool    ev_on = false, ev_held = false, ev_dirty = true, ev_hooked = false;
    char    ev_filter[64] = "";
    u32     x_name = 0, x_first = 0, x_count = 0;
    // The face's part of the session: what it last asked to have kept, by key (a hash of
    // each); and a tab to put up, from what was kept, once the screen is built.
    u64     kept_sent[8] = {};
    char    tab_restore[16] = "";
    // Chain: what its stages' definition was written from; the parameter being dragged.
    u64     chain_sig = 0;
    bool    chain_drag = false;
    u32     chain_drag_stage = 0, chain_drag_param = 0;
    char    chain_keys[kFaceParams][2][16]; // "c_v_S_P", "c_k_S_P": the writer keeps the pointers
    char    stage_keys[8][16];              // "c_flat_S"
};

// ---- the model writer (model.cc) -----------------------------------------------------------
// Each writes the key if its value is not the one last written, and asks for a frame when
// it did. `key` is a string that outlives the face (a literal, or one the face holds).
void face_set(Face& f, const char* key, const char* value);
void face_set(Face& f, const char* key, f64 value);
void face_set_bool(Face& f, const char* key, bool value);
// A list or a table: the document under `root` of a builder, written if its bytes are new.
void face_set_doc(Face& f, const char* key, JsonBuilder& b, JsonRef root);
// Forgets what was written: the next write of every key goes through. Of one key: the
// screen changed it itself (an edit's text).
void face_forget(Face& f);
void face_forget(Face& f, const char* key);

// ---- what the panels share (ui_dev.cc) -----------------------------------------------------
// A node's id as "<prefix><number>" or "<prefix><number>_<number>": the numbers.
bool face_id(Name node, const char* prefix, u64* a, u64* b = nullptr);
// "3:07", "1:02:03"; frames at a rate of 0 are 0:00.
void face_clock(char* out, usize cap, u64 frames, u32 rate);
// What to call a file when nothing better is known: its name, less its directories.
const char* face_file_name(const char* path);

// ---- the panels ----------------------------------------------------------------------------
// Each reads its tables through the context and writes its part of the model; and is
// offered each event of the screen's, saying whether it was its own (and emitted for it).
void transport_sync(TickContext& ctx, Face& f);
bool transport_event(TickContext& ctx, Face& f, const KromeEvent& ev);
void queue_sync(TickContext& ctx, Face& f);
bool queue_event(TickContext& ctx, Face& f, const KromeEvent& ev);
void output_sync(TickContext& ctx, Face& f);
void chain_sync(TickContext& ctx, Face& f);
bool chain_event(TickContext& ctx, Face& f, const KromeEvent& ev);
void scan_sync(TickContext& ctx, Face& f);
void events_sync(TickContext& ctx, Face& f);
bool events_event(TickContext& ctx, Face& f, const KromeEvent& ev);
// The engine's event hook (dev builds), set while the face is up: it keeps what passes.
void events_hook(void* user, const DeliveredEvent& ev);
void tables_sync(TickContext& ctx, Face& f);
bool tables_event(TickContext& ctx, Face& f, const KromeEvent& ev);
void library_sync(TickContext& ctx, Face& f);
bool library_event(TickContext& ctx, Face& f, const KromeEvent& ev);
// The queue's rows in view (and the transport's), asked of the library by their paths; and
// this tick's answers looked up. Before the Transport and the Queue write their titles.
void titles_sync(TickContext& ctx, Face& f);
// What to call a queue row: the library's title for it (with its artist) when it has one
// for this path, else the file's name. `out` is the caller's.
const char* face_title(const LibraryRow* row, const char* path, char* out, usize cap);
// The face's windows onto the library closed (hidden, or going away): asked again when wanted.
void library_close(TickContext& ctx, Face& f);

// ---- the face's part of the session (keep.cc) ----------------------------------------------
// What the face asks the session to keep, when it is not what it last asked: the window's
// size, the tab that is up, and where the library's list is. And what the session kept
// from the last run, taken back.
void keep_sync(TickContext& ctx, Face& f);
void keep_restore(Face& f, const SessionKept& kept);

} // namespace kv
