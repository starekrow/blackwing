// Korvid media_platform: the OS's media surface. What is playing, its state and its
// position go out; the transport's commands come in, from the media keys, the shell's
// controls and anything else that speaks the OS's protocol. It also holds the machine
// awake while asked, and says when the machine is about to sleep and has woken.
//
//   MediaDesc d;
//   d.identity = "Blackwing"; d.notify = wake_my_loop; d.user = me;
//   MediaSurface* s = media_open(d, why, sizeof why);  // null: no surface here, `why` says
//   media_set_track(s, &track);                        // on a track change, in DAC time
//   media_set_status(s, status);                       // on a change of state, and of place
//   media_seeked(s, position_us);                      // after a jump
//   MediaCommand c;
//   while (media_next_command(s, &c)) ...              // in a tick: play, pause, next, seek...
//   media_keep_awake(s, playing);
//   MediaInterruption i;
//   while (media_next_interruption(s, &i)) ...        // the machine going to sleep: pause, then
//   media_sleep_ready(s);                              // let it
//   media_close(s);
//
// It knows nothing of a player: it keeps what it is told, answers the OS from that (the
// position extrapolated from the one given and its rate), and queues what the OS asks.
// Every function may be called from any thread; the OS's calls are answered on a thread of
// the surface's own, and nothing the caller gave runs there but `notify`.
//
// Linux: MPRIS on the session bus, through `dbus`; keep-awake through the desktop portal's
// Inhibit, or logind's where there is no portal; sleep through logind's PrepareForSleep
// and a delay inhibitor. Windows and macOS: not yet (slice 5's milestones 5 and 6), and
// media_open says so. modules/media_platform/README.md has the rest.
#pragma once
#include "korvid/core/types.h"

namespace kv {

enum class MediaState : u8 { Stopped, Playing, Paused };

// What is playing. Strings are UTF-8; "" for what is not known.
struct MediaTrack {
    u64  id = 0;               // stable for the item while it is in the list: the OS's track id is made from it
    char title[256] = "";
    char artist[256] = "";
    char album[256] = "";
    char album_artist[256] = "";
    char path[1024] = "";      // the file, for the OS's url of it
    char art[1024] = "";       // an image file of the cover; "" when there is none
    i64  length_us = 0;        // 0 when it is not known
    u32  track_number = 0;
};

// The transport as the OS should show it.
struct MediaStatus {
    MediaState state = MediaState::Stopped;
    i64  position_us = 0;     // where it was at `at`
    f64  at = 0.0;            // time_now() when that was true
    f64  rate = 0.0;          // how fast it moves: 1 while playing, 0 otherwise
    f64  volume = -1.0;       // 0 to 1 as the listener hears it; less than 0 when there is none to show
    bool can_play = false, can_pause = false, can_next = false, can_previous = false, can_seek = false;
};

enum class MediaCommandKind : u8 {
    Play, Pause, PlayPause, Stop, Next, Previous,
    SeekTo,  // `us`, in the track `track` (0: whatever is playing)
    SeekBy,  // `us` from where it is, either way
    Volume,  // `volume`, 0 to 1
    Raise,   // bring the window up
    Quit,    // end the run
    Count
};
const char* media_command_name(MediaCommandKind k); // "play", "seek to", ...

struct MediaCommand {
    MediaCommandKind kind = MediaCommandKind::Play;
    i64  us = 0;
    u64  track = 0;
    f64  volume = 0.0;
};

enum class MediaInterruptionKind : u8 { Sleep, Wake };
struct MediaInterruption {
    MediaInterruptionKind kind = MediaInterruptionKind::Sleep;
    char reason[96] = "";
};

struct MediaDesc {
    const char* identity = "Blackwing";      // what the OS shows the player as
    const char* desktop_entry = "blackwing"; // the .desktop file's name, without ".desktop"; "" when there is none
    const char* name = "blackwing";          // MPRIS: org.mpris.MediaPlayer2.<name>, and .instance<pid> if that is taken
    bool        can_raise = false;           // there is a window to bring up
    bool        can_quit = true;
    // Where the buses are: null for the session's and the system's own. For tests.
    const char* session_address = nullptr;
    const char* system_address = nullptr;
    bool        watch_sleep = true;          // take a delay inhibitor and hear PrepareForSleep
    void      (*notify)(void* user) = nullptr; // a command or an interruption to take: wake the loop. Any thread
    void*       user = nullptr;
};

// What the surface is doing, for a status line or a panel.
struct MediaInfo {
    char backend[16] = "";       // "mpris"
    char name[128] = "";         // the bus name owned
    bool awake = false;          // the machine is held awake now
    char awake_how[32] = "";     // "portal", "logind"; "" when not
    char awake_why[96] = "";     // why it could not be held, when asked and not
    bool sleep_watched = false;  // PrepareForSleep is heard
    char sleep_why[96] = "";     // why not
    u32  commands = 0;           // taken from the OS since the start
    u32  interruptions = 0;
    char last_command[32] = "";
    u32  clients_asked = 0;      // reads of the surface's properties, answered
};

struct MediaSurface;

MediaSurface* media_open(const MediaDesc& desc, char* why, u32 why_cap);
void          media_close(MediaSurface* s);
// What is playing; null for nothing.
void          media_set_track(MediaSurface* s, const MediaTrack* t);
void          media_set_status(MediaSurface* s, const MediaStatus& st);
// A jump: the OS is told the position moved, at `position_us`, as of `at`.
void          media_seeked(MediaSurface* s, i64 position_us, f64 at);
bool          media_next_command(MediaSurface* s, MediaCommand* out);
bool          media_next_interruption(MediaSurface* s, MediaInterruption* out);
// Hold the machine awake (its idle sleep only; the display may sleep), or let it go.
void          media_keep_awake(MediaSurface* s, bool on);
// After a Sleep interruption, once paused: the machine may sleep now. It is let go after a
// few seconds whether or not this is called, as the OS would anyway.
void          media_sleep_ready(MediaSurface* s);
MediaInfo     media_info(MediaSurface* s);

} // namespace kv
