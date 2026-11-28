-- The conformance walk: every command, notification, table and resource of the player's
-- and the library's API, reached from Lua by the name native code uses.
--
--   args: a long file to play, a directory of tagged files to add as a root, a path that
--   is no file, then the counts the native schemas have (event types, tables and
--   resources, commands), which the surfaces here must match.
--
-- Every name is asked for (kv.has, kv.surface). Every table and resource is read, and has
-- every field its surface lists. Every command is emitted, by name, with its fields; the
-- test's native listeners count them. Every notification is read by name, and those the
-- run brings about are heard with every field their surface lists.
local file, dir, missing = kv.args[1], kv.args[2], kv.args[3]
local want_events, want_tables, want_commands = tonumber(kv.args[4]), tonumber(kv.args[5]), tonumber(kv.args[6])

return kv.steps(function(s)
  local E = kv.enum
  local counts = { events = 0, commands = 0, tables = 0, fields = 0, enums = 0 }
  local events, tables = {}, {}
  for _, name in ipairs { "player", "library" } do
    local surface = kv.surface(name)
    s:check(surface and surface.version == kv.version[name], "the surface " .. name)
    for n, ev in pairs(surface.events) do
      events[n] = ev
      counts.events = counts.events + 1
      if ev.command then counts.commands = counts.commands + 1 end
      s:check(kv.has(n), n .. " is not there by name")
      for _, f in ipairs(ev.fields) do
        s:check(kv.has(n .. "." .. f.name) and type(f.type) == "string", n .. "." .. f.name)
        counts.fields = counts.fields + 1
      end
    end
    for n, t in pairs(surface.tables) do
      tables[n] = t
      counts.tables = counts.tables + 1
      s:check(kv.has(n), n .. " is not there by name")
    end
    for ename, values in pairs(surface.enums) do
      for i, v in ipairs(values) do
        s:check(kv.has(ename .. "." .. v) and E[ename][v] == i - 1 and E[ename][i - 1] == v, ename .. "." .. v)
        counts.enums = counts.enums + 1
      end
    end
    for cname, value in pairs(surface.constants) do s:check(math.type(value) == "integer", cname) end
  end
  s:check(counts.events == want_events, ("the surfaces list %d event types and the schemas have %d"):format(counts.events, want_events))
  s:check(counts.tables == want_tables, ("the surfaces list %d tables and resources and the schemas have %d"):format(counts.tables, want_tables))
  s:check(counts.commands == want_commands, ("the surfaces list %d commands and the schemas have %d"):format(counts.commands, want_commands))
  s:check(not kv.has("PlayerItem.nothing") and not kv.has("PlayerState.Sideways") and not kv.has("no.such.table"), "kv.has says yes to what is not there")
  s:check(kv.has("PlayerItem.size") and kv.has("player.transport.position") and kv.has("PlayerState.Paused") and kv.has("player.params"), "kv.has")

  -- Every table and resource, read by its name: every field its surface lists is there.
  for name, t in pairs(tables) do
    if t.resource then
      local record = s:resource(name)
      for _, f in ipairs(t.fields) do s:check(record[f.name] ~= nil, name .. " has no " .. f.name) end
    else
      local kt = s:ktable(name)
      local have = {}
      for _, f in ipairs(kt:fields()) do have[f] = true end
      for _, f in ipairs(t.fields) do s:check(have[f.name] or have[t.component .. "." .. f.name], name .. " has no " .. f.name) end
      s:check(kt:name() == name and kt:live() >= 0 and kt:capacity() > 0, name)
    end
  end

  -- Every notification, read by its name; and each one heard has the fields its surface lists.
  local heard = {}
  local function hear(type_name, pred, timeout)
    local ev = s:wait(type_name, pred, timeout)
    s:check(ev ~= nil, type_name .. " did not come")
    for _, f in ipairs(events[type_name].fields) do s:check(ev[f.name] ~= nil, type_name .. " came without " .. f.name) end
    heard[type_name] = true
    return ev
  end

  -- Every command, by its name. The library's first.
  local emitted = {}
  local function emit(type_name, fields) s:emit(type_name, fields); emitted[type_name] = true end
  emit("LibraryAddRoot", { path = dir })
  local aside = hear("LibraryOverridesSetAside", nil, 10) -- the test left a file there that will not parse
  s:check(aside.root == 1 and #aside.aside > 0 and #aside.reason > 0, "the override file set aside")
  hear("LibraryChanged")
  local finished = hear("LibraryScanFinished", nil, 30)
  s:check(finished.root == 1 and finished.tracks == 24, "the root's scan")
  emit("LibraryQuery", { id = 1, view = E.LibraryView.Tracks, sort = E.LibrarySort.Title, count = 5 })
  local result = hear("LibraryResult", function(ev) return ev.id == 1 end)
  s:check(result.total == 24 and result.count == 5, "the window")
  local rows, row = s:ktable("library.rows"), nil
  for h in rows:each() do
    local r = rows:row(h)
    if r.query == 1 and r.index == 0 then row = { id = r.id, path = r.path, title = r.title, kind = r.kind } end
  end
  s:check(row and row.kind == E.LibraryRowKind.Track and #row.path > 0 and #row.title > 0, "the window's first row")
  emit("LibraryPrioritize", { query = 1 })
  emit("LibraryPath", { id = 2, index = 7, path = row.path })
  hear("LibraryResult", function(ev) return ev.id == 2 end)
  emit("LibraryOverride", { track = row.id, field = "title", value = "Walked" })
  hear("LibraryChanged")
  emit("LibraryClose", { id = 2 })
  emit("LibraryClose", { id = 1 })

  -- The player's. A long file, played; then every way of moving.
  emit("PlayerEnqueue", { path = file })
  emit("PlayerPlay")
  local started = hear("PlayerTrackStarted")
  s:check(started.item == 1 and started.rate == 8000, "what started")
  emit("PlayerSeek", { item = 0, frame = 4000 })
  s:check(hear("PlayerSeeked").frame == 4000, "the seek")
  emit("PlayerSeekBy", { seconds = 0.5 })
  local by = hear("PlayerSeeked").frame -- half a second on from the frame heard: 4000 frames at 8 kHz past where it was
  s:check(by >= 8000 and by < 12000, "the seek by seconds")
  -- The devices: the capture device is a row, open; choosing it again moves the music to it.
  local devices, capture = s:ktable("player.devices"), nil
  for h in devices:each() do
    local d = devices:row(h)
    if d.name == "capture" then capture = d end
  end
  s:check(capture and capture.present == 1 and capture.open == 1, "the capture device's row")
  emit("PlayerSelectDevice", { name = "capture" })
  local moved = hear("PlayerDeviceChanged", function(ev) return ev.kind == E.PlayerDeviceChange.Moved end)
  s:check(moved.name == "capture", "the music moved to the device chosen")
  s:check(s:resource("player.output").chosen == "capture", "chosen")
  hear("PlayerSeeked") -- on from where it was heard
  emit("PlayerSetParam", { stage = 0, param = 0, value = -3.0 })
  s:check(hear("PlayerParamChanged", function(ev) return ev.stage == 0 and ev.param == 0 end).value == -3.0, "the volume")
  emit("PlayerApplyPreset", { stage = 1, preset = 1 })
  s:check(hear("PlayerParamChanged", function(ev) return ev.stage == 1 and ev.param == 2 end).value == 6.0, "the preset's first band")
  emit("PlayerPause")
  s:wait(0.1)
  s:check(s:resource("player.transport").state == E.PlayerState.Paused, "paused")
  -- A place taken up without playing: held at a row and a frame.
  emit("PlayerCue", { item = 1, frame = 16000, frames = 32000, rate = 8000 })
  s:wait(0.1)
  local cued = s:resource("player.transport")
  s:check(cued.state == E.PlayerState.Paused and cued.item == 1 and cued.position == 16000, "cued")
  -- What the library collects is queued (the application's glue); a file that is not there fails alone.
  emit("LibraryCollect", { id = 3, kind = E.LibraryRowKind.Track, row = row.id })
  local got = hear("LibraryCollected", function(ev) return ev.id == 3 end)
  s:check(got.total == 1 and got.path == row.path, "what was collected")
  emit("PlayerEnqueue", { path = missing })
  s:wait(0.2)
  s:check(s:resource("player.transport").queued == 3, "three rows queued")
  emit("PlayerQueueEdit", { op = E.PlayerEditOp.Move, item = 3, before = 2 }) -- the missing file, before the collected one
  emit("PlayerJump", { item = 3 })
  emit("PlayerPlay") -- on from the pause
  s:check(hear("PlayerTrackFailed").item == 3, "the file that is not there")
  hear("PlayerTrackStarted", function(ev) return ev.item == 2 end)          -- and the next plays
  hear("PlayerTrackEnded", function(ev) return ev.item == 2 end)
  hear("PlayerQueueEnded")
  emit("PlayerPrevious")
  emit("PlayerNext")
  emit("PlayerStop")
  emit("PlayerQueueEdit", { op = E.PlayerEditOp.Clear })
  s:wait(0.2)
  s:check(s:resource("player.transport").queued == 0 and s:ktable("player.queue"):live() == 0, "the queue cleared")
  emit("LibraryRemoveRoot", { root = 1 })
  s:wait(0.3)
  s:check(s:ktable("library.roots"):live() == 0, "the root removed")

  -- Every command was emitted; the notifications a run cannot bring about on purpose
  -- (an underrun) are read by name all the same, which the helper under this does each slice.
  for n, ev in pairs(events) do
    if ev.command then s:check(emitted[n], n .. " was never emitted") end
  end
  local n_heard = 0
  for _ in pairs(heard) do n_heard = n_heard + 1 end
  print(("walked: %d event types (%d commands emitted, %d notifications heard with their fields), %d tables and resources, %d fields, %d enum values"):format(
    counts.events, counts.commands, n_heard, counts.tables, counts.fields, counts.enums))
  -- Every notification but the one a run cannot bring about on purpose was heard.
  for n, ev in pairs(events) do
    if not ev.command and n ~= "HifiUnderrun" then s:check(heard[n], n .. " was never heard") end
  end
  return 0
end)
