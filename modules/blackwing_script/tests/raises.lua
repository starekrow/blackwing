-- A script that raises, on a line the test knows: said with its file and line, and
-- stopped. The player is not.
local n = 0
return {
  name = "raises",
  systems = {
    { name = ".tick", every = 0, emits = "PlayerEnqueue", fn = function(ctx)
        n = n + 1
        if n == 3 then
          local nothing = nil
          return nothing.field -- line 11: indexes nil
        end
      end },
  },
}
