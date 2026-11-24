-- The record, as the spec has it: a handler on an event, a system on a period, startup,
-- and the rules a unit is held to. Ends the run with 7 when all of it held.
local file = kv.args[1]
local started, ticks, held = 0, 0, {}

local function check(ok, what) if not ok then error("record.lua: " .. what, 2) end end

local function on_started(ctx, batch)
  for _, ev in ipairs(batch) do
    started = started + 1
    -- The handler's own event, its fields by name; and what it said it reads.
    check(ev.item == 1 and ev.rate == 8000 and ev.channels == 2 and ev.bits == 16 and ev.seq > 0, "PlayerTrackStarted's fields")
    local t = ctx:resource("player.transport")
    check(t.state == kv.enum.PlayerState.Playing and t.item == 1 and t.rate == 8000, "the transport when a track has started")
    -- A resource and a row that are not the script's own are read-only.
    check(not pcall(function() t.position = 0 end), "a resource written")
    local q = ctx:ktable("player.queue")
    check(q:live() == 1 and q:name() == "player.queue", "the queue")
    for h in q:each() do
      local row = q:row(h)
      check(row.path == file and row.state == kv.enum.PlayerItemState.Playing and kv.enum.PlayerItemState.name(row.state) == "playing", "the queue's row")
      check(not pcall(function() row.pos = 9 end), "a row written")
    end
    check(not pcall(function() q:spawn({}) end) and not pcall(function() q:destroy(1) end), "a table written")
    -- What a unit did not name, it does not reach: each an error at the call.
    check(not pcall(function() ctx:emit("PlayerStop", {}) end), "an emit that was not declared")
    check(not pcall(function() ctx:resource("player.output") end), "a read that was not declared")
    check(not pcall(function() ctx:received("PlayerSeeked") end), "a batch that was not declared")
    -- A field that does not fit is an error, never a quiet change, and nothing is emitted.
    check(not pcall(function() ctx:emit("PlayerSeekBy", { seconds = "ten" }) end), "a wrong type")
    check(not pcall(function() ctx:emit("PlayerSeekBy", { second = 10 }) end), "an unknown field")
    ctx:emit("PlayerSeekBy", { seconds = 1.0 })
    held.handler = true
  end
end

local function every(ctx)
  ticks = ticks + 1
  if ticks == 1 then
    check(ctx.slice >= 1 and ctx.now > 0, "ctx.now and ctx.slice")
    ctx:emit("PlayerEnqueue", { path = file })
    ctx:emit("PlayerPlay")
  end
  for _, ev in ipairs(ctx:received("PlayerSeeked")) do
    check(ev.item == 1 and ev.frame >= 8000, "the seek the handler asked for")
    held.seeked = true
  end
  if held.handler and held.seeked and held.startup then
    check(started == 1, "one track started")
    kv.quit(7)
  end
end

return {
  name = "record",
  handlers = {
    { name = ".started", event = "PlayerTrackStarted", fn = on_started, reads = "player.transport, player.queue", emits = "PlayerSeekBy" },
  },
  systems = {
    { name = ".every", fn = every, every = 0.05, reads = "PlayerSeeked", emits = "PlayerEnqueue, PlayerPlay" },
  },
  startup = function(ctx)
    check(kv.version.script == "0.1.0" and kv.version.player == "0.1.0" and kv.version.library == "0.1.0", "kv.version")
    check(type(kv.json) == "table" and type(kv.sha256) == "function" and io == nil and os == nil and dofile == nil and require == nil, "the libraries")
    held.startup = true
    return true
  end,
}
