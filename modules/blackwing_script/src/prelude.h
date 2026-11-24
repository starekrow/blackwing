// The host's Lua, run once before the user's script: the surface as `kv` shows it, the
// `ctx` a unit is handed, the record checked and its units run. It is Lua because that is
// the shortest way to say it; the binding under it (events, rows, resources by their field
// tables) is the engine's, in C++.
//
// It is called with one table, `host`: kv (the table the script will see), say and hear
// (the engine's events objects: one that emits outside the schedule, one that is the
// host's unit's), ktable(name), resource(name), quit(code), out(level, text), surfaces,
// args. It returns { load = function(record), slice = function(now) }.
#pragma once

namespace kv {

constexpr const char* k_script_prelude = R"lua(
local host = ...
local kv = host.kv
local surfaces = host.surfaces
local emitted = 0

-- ---- kv -----------------------------------------------------------------------------------

kv.version = { script = "0.1.0" }
for name, s in pairs(surfaces) do kv.version[name] = s.version end
kv.args = host.args

function kv.log(level, text)
  if level ~= "info" and level ~= "warn" and level ~= "error" then error("kv.log takes \"info\", \"warn\" or \"error\", then the text", 2) end
  host.out(level, tostring(text))
end
print = function(...)
  local parts = table.pack(...)
  for i = 1, parts.n do parts[i] = tostring(parts[i]) end
  host.out("info", table.concat(parts, "\t"))
end

function kv.surface(name) return surfaces[name] end

-- Every enum of every surface, both ways, and its values' lower-case names.
kv.enum = {}
for _, s in pairs(surfaces) do
  for ename, values in pairs(s.enums) do
    local t = {}
    for i, v in ipairs(values) do t[v] = i - 1; t[i - 1] = v end
    t.name = function(v) local n = values[(v or -1) + 1]; return n and n:lower() or nil end
    kv.enum[ename] = t
  end
end

-- What there is, by name: an event type, a table, a resource, a field of any of them
-- ("PlayerItem.size", "PlayerSeek.frame", "player.transport.position"), an enum's value
-- ("PlayerState.Paused"). A capability is granted here whatever it is: the script is the
-- user's own.
local events, tables, fields_of = {}, {}, {}
for _, s in pairs(surfaces) do
  for n, ev in pairs(s.events) do events[n] = ev; fields_of[n] = ev.fields end
  for n, t in pairs(s.tables) do
    tables[n] = t
    fields_of[n] = t.fields
    if t.component ~= "" then fields_of[t.component] = t.fields end
  end
end
local capabilities = { ["player.transport"] = true, ["player.queue"] = true, ["player.params"] = true, ["library.query"] = true,
                       ["library.override"] = true, ["library.roots"] = true, ["hifi.stage"] = true, ["hifi.decoder"] = true, publish = true }
function kv.has(name)
  if type(name) ~= "string" then return false end
  if events[name] or tables[name] or capabilities[name] then return true end
  local owner, member = name:match("^(.*)%.([^.]+)$")
  if not owner then return false end
  local fields = fields_of[owner]
  if fields then for _, f in ipairs(fields) do if f.name == member then return true end end end
  local e = kv.enum[owner]
  return e ~= nil and type(e[member]) == "number"
end

-- ---- ctx ----------------------------------------------------------------------------------

local function names(text, what, unit)
  local set = {}
  if text == nil then return set end
  if type(text) ~= "string" then error(("%s: `%s` is a string of names with commas between"):format(unit, what), 0) end
  for n in text:gmatch("[^,%s]+") do set[n] = true end
  return set
end

-- A record that reads, and says so when written to.
local function readonly(t, what)
  return setmetatable({}, {
    __index = t,
    __newindex = function() error(what .. " is read-only: it is not this script's own", 2) end,
    __pairs = function() return next, t, nil end,
  })
end

-- A table of someone else's: every read the engine's table object has, and no write.
local function table_view(kt, name)
  local v = {}
  function v:each() return kt:each() end
  function v:row(h)
    local r = kt:row(h)
    if r == nil then return nil end
    return setmetatable({}, {
      __index = function(_, k) return r[k] end,
      __newindex = function() error(name .. " is read-only: it is not this script's own", 2) end,
    })
  end
  function v:get(h, field) return kt:get(h, field) end
  function v:live() return kt:live() end
  function v:capacity() return kt:capacity() end
  function v:name() return kt:name() end
  function v:fields() return kt:fields() end
  function v:valid(h) return kt:valid(h) end
  for _, m in ipairs { "spawn", "set", "destroy" } do
    v[m] = function() error(("%s: %s is read-only: it is not this script's own"):format(m, name), 2) end
  end
  return v
end

local function make_ctx(unit)
  local c = { now = 0, slice = 0 }
  function c:emit(type_name, fields)
    if not unit.emits[type_name] then error(("%s emits %s without naming it in `emits`"):format(unit.name, tostring(type_name)), 2) end
    if fields ~= nil and type(fields) ~= "table" then error("ctx:emit takes an event type's name and a table of its fields", 2) end
    local ok, why = pcall(host.say.emit, host.say, type_name, fields or {})
    if not ok then error(why, 2) end
    emitted = emitted + 1
  end
  function c:received(type_name)
    if not unit.reads[type_name] then error(("%s reads %s without naming it in `reads`"):format(unit.name, tostring(type_name)), 2) end
    -- A system's batch is what has come since it last ran, which may be many ticks ago.
    if unit.inbox then return unit.inbox[type_name] or {} end
    return host.hear:received(type_name)
  end
  function c:resource(name)
    if not unit.reads[name] then error(("%s reads %s without naming it in `reads`"):format(unit.name, tostring(name)), 2) end
    local r = host.resource(name)
    if r == nil then error(("there is no resource '%s'"):format(tostring(name)), 2) end
    return readonly(r, name)
  end
  function c:ktable(name)
    if not unit.reads[name] then error(("%s reads %s without naming it in `reads`"):format(unit.name, tostring(name)), 2) end
    return table_view(host.ktable(name), name)
  end
  return c
end

-- ---- the record ---------------------------------------------------------------------------

local record, handlers, systems, started = nil, {}, {}, false
local nslice = 0

local function unit_of(u, kind, i, base)
  if type(u) ~= "table" then error(("%s %d of the record is not a table"):format(kind, i), 0) end
  local name = u.name
  if type(name) ~= "string" then error(("%s %d of the record has no name"):format(kind, i), 0) end
  if name:sub(1, 1) == "." then name = base .. name end
  if type(u.fn) ~= "function" then error(("%s: its `fn` is not a function"):format(name), 0) end
  local unit = { name = name, fn = u.fn, reads = names(u.reads, "reads", name), emits = names(u.emits, "emits", name) }
  for n in pairs(unit.reads) do
    if not events[n] and not tables[n] then error(("%s reads %s, which no surface has"):format(name, n), 0) end
  end
  for n in pairs(unit.emits) do
    if not events[n] then error(("%s emits %s, which is no event type"):format(name, n), 0) end
  end
  return unit
end

local function load(rec)
  if type(rec) ~= "table" then error("the script returns its record: a table with handlers and systems (docs/specs/script-modules.md)", 0) end
  local base = type(rec.name) == "string" and rec.name or "script"
  for i, h in ipairs(rec.handlers or {}) do
    local unit = unit_of(h, "handler", i, base)
    if type(h.event) ~= "string" or not events[h.event] then error(("%s: its `event` is %s, which is no event type"):format(unit.name, tostring(h.event)), 0) end
    unit.event = h.event
    unit.reads[h.event] = true
    unit.ctx = make_ctx(unit)
    handlers[#handlers + 1] = unit
  end
  for i, s in ipairs(rec.systems or {}) do
    local unit = unit_of(s, "system", i, base)
    unit.every = tonumber(s.every)
    unit.on_change = s.on_change == true
    if not unit.every and not unit.on_change then error(("%s: a system has `every` (seconds) or `on_change = true`"):format(unit.name), 0) end
    unit.due = 0
    unit.inbox = {}
    unit.ctx = make_ctx(unit)
    systems[#systems + 1] = unit
  end
  for _, f in ipairs { "startup", "shutdown" } do
    if rec[f] ~= nil and type(rec[f]) ~= "function" then error(("the record's `%s` is not a function"):format(f), 0) end
  end
  record = rec
  return #handlers, #systems
end

-- One slice: the handlers whose event came, in the record's order, then the systems that
-- are due. Returns how long until a system is next due (-1: none is waiting on the clock),
-- and how many events have been emitted so far.
local function slice(now)
  nslice = nslice + 1
  if not started then
    started = true
    if record.startup then
      local c = make_ctx({ name = "startup", reads = setmetatable({}, { __index = function() return true end }), emits = setmetatable({}, { __index = function() return true end }) })
      c.now, c.slice = now, nslice
      if record.startup(c) == false then error("the record's startup returned false", 0) end
    end
  end
  for _, h in ipairs(handlers) do
    local batch = host.hear:received(h.event)
    if #batch > 0 then
      h.ctx.now, h.ctx.slice = now, nslice
      h.fn(h.ctx, batch)
    end
  end
  local soonest = -1
  for _, s in ipairs(systems) do
    -- What it reads that came in this tick is kept for it until it runs.
    local came = false
    for n in pairs(s.reads) do
      if events[n] then
        local batch = host.hear:received(n)
        if #batch > 0 then
          came = true
          local kept = s.inbox[n]
          if not kept then kept = {}; s.inbox[n] = kept end
          for _, ev in ipairs(batch) do
            if #kept < 4096 then kept[#kept + 1] = ev end -- a system that never runs does not keep everything for ever
          end
        end
      end
    end
    local run = false
    if s.every then
      if now >= s.due then run = true; s.due = now + s.every end
      local wait = s.due - now
      if soonest < 0 or wait < soonest then soonest = wait end
    end
    if s.on_change and came then run = true end
    if run then
      s.ctx.now, s.ctx.slice = now, nslice
      s.fn(s.ctx)
      s.inbox = {}
    end
  end
  return soonest, emitted
end

-- ---- kv.quit and kv.steps -------------------------------------------------------------------

-- Ends the run, with a code: a script the user runs is a command, and a test is a script.
function kv.quit(code)
  if code ~= nil and math.type(code) ~= "integer" then error("kv.quit takes an exit code: an integer, or nothing for 0", 2) end
  host.quit(code or 0)
end

-- A script that reads top to bottom, over the record: kv.steps(function(s) ... end) is a
-- record with one system that runs the function as a coroutine. In it:
--   s:emit(type, fields)                  as ctx:emit
--   s:resource(name), s:ktable(name)      as ctx's
--   s:wait(seconds)                       comes back after that long
--   s:wait(type [, pred] [, timeout])     the next event of the type not yet waited for (for
--                                         which pred(ev) holds, if given); nil after timeout
--                                         seconds (10 if not said)
--   s:check(ok, text)                     raises text when ok is false
-- When the function returns, the run ends: with the integer it returned, or 0.
function kv.steps(body)
  if type(body) ~= "function" then error("kv.steps takes a function", 2) end
  local reads, emits, heard = {}, {}, {}
  for _, s in pairs(surfaces) do
    for n in pairs(s.events) do reads[#reads + 1] = n; emits[#emits + 1] = n; heard[n] = {} end
    for n in pairs(s.tables) do reads[#reads + 1] = n end
  end
  local co = coroutine.create(body)
  local pending, ctx, now = nil, nil, 0
  local s = {}
  function s:emit(type_name, fields) return ctx:emit(type_name, fields) end
  function s:resource(name) return ctx:resource(name) end
  function s:ktable(name) return ctx:ktable(name) end
  function s:now() return now end
  function s:check(ok, text) if not ok then error(text or "a check did not hold", 2) end return ok end
  function s:wait(what, a, b)
    if type(what) == "number" then return coroutine.yield({ until_ = now + what }) end
    if not heard[what] then error(("s:wait: %s is no event type"):format(tostring(what)), 2) end
    local pred, timeout = a, b
    if type(a) == "number" then pred, timeout = nil, a end
    return coroutine.yield({ type = what, pred = pred, until_ = now + (timeout or 10) })
  end
  local function step(c)
    ctx, now = c, c.now
    for n, list in pairs(heard) do
      for _, ev in ipairs(c:received(n)) do
        list[#list + 1] = ev
        if #list > 1024 then table.remove(list, 1) end
      end
    end
    while true do
      local give
      if pending then
        if pending.type then
          local list, found = heard[pending.type], nil
          for i, ev in ipairs(list) do
            if not pending.pred or pending.pred(ev) then found = i break end
          end
          if found then
            give = list[found]
            for _ = 1, found do table.remove(list, 1) end -- it, and what came before it, are waited for
          elseif now < pending.until_ then
            return
          end
        elseif now < pending.until_ then
          return
        else
          give = true
        end
      end
      local ok, out
      if pending == nil then ok, out = coroutine.resume(co, s) else ok, out = coroutine.resume(co, give) end
      if not ok then error(out, 0) end
      if coroutine.status(co) == "dead" then
        kv.quit(math.type(out) == "integer" and out or 0)
        pending = { until_ = math.huge }
        return
      end
      pending = out
      if type(pending) ~= "table" then error("kv.steps: the function yielded something that is not a wait", 0) end
    end
  end
  return { name = "steps", systems = { { name = ".run", fn = step, every = 0, reads = table.concat(reads, ", "), emits = table.concat(emits, ", ") } } }
end

return { load = load, slice = slice, shutdown = function() if record and record.shutdown and started then record.shutdown(make_ctx({ name = "shutdown", reads = {}, emits = {} })) end end }
)lua";

} // namespace kv
