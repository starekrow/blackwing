-- ui_dev's behaviours. dev-slider: std's slider over 0 to 1, saying what is done to it.
-- The value it holds is where the knob is; what the value means is its owner's business.

local function clamp(v, lo, hi) if v < lo then return lo elseif v > hi then return hi end return v end

local knob = 14

local function at(w, x)
  return clamp((x - knob / 2) / math.max(1, w.rect.w - knob), 0, 1)
end

local function value(w) return clamp(tonumber(w.value) or 0, 0, 1) end

local function move(w, v)
  v = clamp(v, 0, 1)
  w:set(v)
  w:emit("slide", v)
end

local slider = {
  press = function(w, x) move(w, at(w, x)) end,
  drag = function(w, x) move(w, at(w, x)) end,
  release = function(w) w:emit("slide_end", value(w)) end,
  key = function(w, name)
    local step = w.step > 0 and w.step or 0.01
    local v = value(w)
    if name == "left" or name == "down" then v = v - step
    elseif name == "right" or name == "up" then v = v + step
    elseif name == "pagedown" then v = v - 0.1
    elseif name == "pageup" then v = v + 0.1
    elseif name == "home" then v = 0
    elseif name == "end" then v = 1
    else return false end
    move(w, v)
    w:emit("slide_end", value(w))
    return true
  end,
  wheel = function(w, dy)
    local step = w.step > 0 and w.step or 0.01
    move(w, value(w) + (dy > 0 and step or -step))
    w:emit("slide_end", value(w))
    return true
  end,
  update = function(w)
    local pct = string.format("%.3f%%", value(w) * 100)
    w:part("fill", "w", pct)
    w:part("lead", "w", pct)
  end,
}

return { ["dev-slider"] = slider }
