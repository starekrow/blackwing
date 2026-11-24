// The glue between the player and the library, as two handlers. The player is a player
// with no library and the library a library with no player; an application that has both
// puts this module in its engine, and then:
//
//   - a file queued is the library's priority (PlayerEnqueue, heard as the player hears
//     it, becomes LibraryPrioritize for its path): its tags are read next;
//   - what the library collects is queued (each LibraryCollected becomes a PlayerEnqueue,
//     in the order they come): queueing a folder, an album, an artist, a genre or a search
//     is one command to the library, from a face or a script, and this is its other half.
#include "korvid/blackwing_glue/module.h"
#include "korvid/blackwing_library/library.h"
#include "korvid/blackwing_player/player.h"
#include "korvid/core/engine.h"
#include <string.h>

namespace kv {

namespace {

void glue_enqueued(TickContext& ctx, Span<const PlayerEnqueue> events) {
    for (const PlayerEnqueue& e : events) {
        LibraryPrioritize p;
        usize n = sizeof p.path < sizeof e.path ? sizeof p.path : sizeof e.path;
        memcpy(p.path, e.path, n);
        p.path[n - 1] = 0;
        ctx.emit(p);
    }
}

void glue_collected(TickContext& ctx, Span<const LibraryCollected> events) {
    for (const LibraryCollected& e : events) {
        if (!e.path[0]) continue; // nothing was found
        PlayerEnqueue q{};
        usize n = sizeof q.path < sizeof e.path ? sizeof q.path : sizeof e.path;
        memcpy(q.path, e.path, n);
        q.path[n - 1] = 0;
        ctx.emit(q);
    }
}

constinit const HandlerDesc k_handlers[] = {
    {.name = ".enqueued", KV_ON(PlayerEnqueue, glue_enqueued), .phase = Phase::PreSim, .emits = "LibraryPrioritize"},
    // After the sim, so what it emits is next tick's: an enqueue is heard by the handler
    // above, which the library hears in turn, and that cannot all be one phase.
    {.name = ".collected", KV_ON(LibraryCollected, glue_collected), .phase = Phase::PostSim, .emits = "PlayerEnqueue"},
};

} // namespace

KV_MODULE_DEFINE(blackwing_glue_module) = {
    .name     = "glue",
    .handlers = k_handlers,
};

} // namespace kv
