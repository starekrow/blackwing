// What every backend shares: the commands' names, and on an OS with no backend yet, a
// surface that is not there and says so.
#include "korvid/media_platform/media.h"
#include "korvid/core/format.h"

namespace kv {

const char* media_command_name(MediaCommandKind k) {
    switch (k) {
    case MediaCommandKind::Play:      return "play";
    case MediaCommandKind::Pause:     return "pause";
    case MediaCommandKind::PlayPause: return "play/pause";
    case MediaCommandKind::Stop:      return "stop";
    case MediaCommandKind::Next:      return "next";
    case MediaCommandKind::Previous:  return "previous";
    case MediaCommandKind::SeekTo:    return "seek to";
    case MediaCommandKind::SeekBy:    return "seek by";
    case MediaCommandKind::Volume:    return "volume";
    case MediaCommandKind::Raise:     return "raise";
    case MediaCommandKind::Quit:      return "quit";
    default:                          return "?";
    }
}

#if !defined(__linux__)
// Windows (SMTC) and macOS (Now Playing) are slice 5's milestones 5 and 6.
MediaSurface* media_open(const MediaDesc&, char* why, u32 cap) {
    kv::format(why, cap, "no media surface on this OS yet");
    return nullptr;
}
void      media_close(MediaSurface*) {}
void      media_set_track(MediaSurface*, const MediaTrack*) {}
void      media_set_status(MediaSurface*, const MediaStatus&) {}
void      media_seeked(MediaSurface*, i64, f64) {}
bool      media_next_command(MediaSurface*, MediaCommand*) { return false; }
bool      media_next_interruption(MediaSurface*, MediaInterruption*) { return false; }
void      media_keep_awake(MediaSurface*, bool) {}
void      media_sleep_ready(MediaSurface*) {}
MediaInfo media_info(MediaSurface*) { return MediaInfo{}; }
#endif

} // namespace kv
