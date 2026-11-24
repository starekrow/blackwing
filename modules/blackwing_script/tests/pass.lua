-- A script doing, with no face, what the face does: adds a root, finds a track, plays it,
-- seeks, sets a band, reads the transport, and ends the run with a code. Along the way it
-- checks what slices 1 to 3 promised that a script can see.
--
--   args: a directory of tagged files; a longer file, to seek in.
local dir, long = kv.args[1], kv.args[2]

return kv.steps(function(s)
  local E = kv.enum
  local function transport() return s:resource("player.transport") end
  local function output() return s:resource("player.output") end

  -- Slice 3: aim it at a directory and it is done. Every file a row; found by a word.
  s:emit("LibraryAddRoot", { path = dir })
  local scan = s:wait("LibraryScanFinished", 30)
  s:check(scan and scan.tracks == 24, "the scan did not finish with every track")
  s:check(s:resource("library.scan").unread == 0, "tags still unread after the scan finished")
  s:emit("LibraryQuery", { id = 1, view = E.LibraryView.Search, text = "zq17", count = 10 })
  local found = s:wait("LibraryResult", function(ev) return ev.id == 1 end)
  s:check(found and found.total == 1, "a search for one track's word did not find one")
  local row
  local rows = s:ktable("library.rows")
  for h in rows:each() do local r = rows:row(h); if r.query == 1 then row = { id = r.id, path = r.path, title = r.title, artist = r.artist, frames = r.frames, rate = r.rate } end end
  s:check(row and row.title:find("Zq17") and row.rate == 44100 and row.frames == 4096, "the found track's tags")

  -- Slice 1: sound out. Played from the library's row; started in the device's time.
  s:emit("LibraryCollect", { id = 9, kind = E.LibraryRowKind.Track, row = row.id })
  s:check(s:wait("LibraryCollected", function(ev) return ev.id == 9 end), "the row was not collected")
  s:emit("PlayerEnqueue", { path = long })
  s:emit("PlayerPlay")
  local first = s:wait("PlayerTrackStarted")
  s:check(first and first.item == 1 and first.rate == 44100 and first.frames == 4096, "the library's track did not start as its header says")
  s:check(output().exact == 1 and output().shared == 0, "an unshared device with a flat chain is exact")
  local ended = s:wait("PlayerTrackEnded")
  s:check(ended and ended.item == 1 and ended.frames == 4096 and not ended.damaged, "it did not end whole")
  -- Gapless into another format: the next starts, and every event is in order by seq.
  local second = s:wait("PlayerTrackStarted")
  s:check(second and second.item == 2 and second.rate == 8000 and second.seq > ended.seq, "the second track")

  -- Slice 2: the recut. A seek is heard as a seek; a band set is heard, and exact says so.
  s:emit("PlayerSeek", { item = 0, frame = 16000 })
  local seeked = s:wait("PlayerSeeked")
  s:check(seeked and seeked.item == 2 and seeked.frame == 16000, "the seek")
  s:check(transport().position >= 16000 and transport().seek_item == 0, "the position after the seek")
  s:emit("PlayerSetParam", { stage = 1, param = 2, value = 6.0 })
  local band = s:wait("PlayerParamChanged")
  s:check(band and band.stage == 1 and band.param == 2 and band.value == 6.0, "the band")
  local chain, flat = s:ktable("player.chain"), {}
  for h in chain:each() do local r = chain:row(h); flat[r.name] = r.flat end
  s:check(flat.gain == 1 and flat.eq == 0 and output().exact == 0 and output().recuts >= 1, "the chain after a band is set")
  s:emit("PlayerPause")
  s:wait(0.15)
  local held = transport().position
  s:wait(0.15)
  s:check(transport().state == E.PlayerState.Paused and transport().position == held, "paused, the position holds")
  s:emit("PlayerPlay")
  s:check(s:wait("PlayerQueueEnded", 10), "the queue did not end")
  s:check(transport().state == E.PlayerState.Stopped and transport().left == 0 and output().underruns == 0, "at the end: stopped, nothing left, no underrun")
  print(("pass: played \"%s\" by %s from the library, then sought to 2.0 s of the next and set a band; no underrun"):format(row.title, row.artist))
  return 0
end)
