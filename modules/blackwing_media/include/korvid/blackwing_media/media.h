// Korvid blackwing_media: the OS's media surface on the busses.
//
// In: what the OS asks becomes what a face would emit: PlayerPlay, PlayerPause,
// PlayerStop, PlayerNext, PlayerPrevious, PlayerSeek, PlayerSeekBy, and PlayerSetParam on
// the gain stage for a volume. MPRIS's Quit ends the run; Raise is MediaRaise, for
// whoever has the window.
//
// Out: the track heard (PlayerTrackStarted, so in DAC time), titled by the library and
// with its art; the state; the position with its rate, sent on a change and on a seek
// (PlayerSeeked), never per tick.
//
// Keep-awake while the transport plays, let go a few seconds after it stops. The machine
// going to sleep is MediaInterrupted: playing, it is paused first, and the machine let
// sleep once it is. Waking is MediaResumed. Nothing resumes on its own.
#pragma once
#include "korvid/core/types.h"

namespace kv {

struct Engine;

// The OS asked for the window: bring it up. (A run with no window says it cannot raise.)
struct MediaRaise { u8 unused = 0; };
// The machine is going to sleep (or another interruption, in words); and back.
struct MediaInterrupted { char reason[96]; };
struct MediaResumed     { char reason[96]; };

// The resource "media.status".
struct MediaStatusRow {
    u8   open = 0;            // a surface is up
    u8   awake = 0;           // the machine is held awake
    u8   sleep_watched = 0;   // the machine's sleep is heard
    u8   playing = 0;         // what the OS is shown: the transport plays
    u32  commands = 0;        // from the OS
    u32  interruptions = 0;
    u32  clients_asked = 0;   // the OS's reads of what is shown
    char backend[16] = "";    // "mpris"
    char name[128] = "";      // the bus name owned
    char awake_how[32] = "";  // "portal", "logind"
    char why[96] = "";        // why there is no surface, or why it is not awake while playing
    char last_command[32] = "";
    char title[256] = "";     // what the OS is shown as playing
    char art[512] = "";       // the cover's file; "" for none
};

struct MediaConfig {
    bool        enabled = false;            // off unless asked: a test or a tool owns no name on the desktop's bus
    bool        can_raise = false;          // a window to bring up
    const char* session_address = nullptr;  // tests: a private bus; null, the session's
    const char* system_address = nullptr;
    bool        watch_sleep = true;
    f64         awake_linger = 5.0;         // seconds held after the transport stops playing: a pause between tracks is not a pause
    const char* name = "blackwing";         // MPRIS's org.mpris.MediaPlayer2.<name>
    const char* art_dir = nullptr;          // where art from the tags is written; null: the cache directory's art/
};
void media_configure(const MediaConfig& config);
// The module's "media.status", for code that has the engine (a command, a test); null
// without the module.
const MediaStatusRow* media_status(Engine& e);

// The art for a file: an image beside it (cover, folder, front, album; jpg or png), else
// the front cover in its tags, written once to `art_dir`. False when it has none. Blocking:
// the module calls it on a thread of its own.
bool media_find_art(const char* path, const char* art_dir, char* out, u32 cap);

} // namespace kv
