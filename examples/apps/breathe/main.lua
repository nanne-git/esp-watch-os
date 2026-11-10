-- Breathe: timers, an animated arc and keep_awake.
local PHASES = {
  {text = "Breathe in", secs = 4, color = 0x40C8E0},
  {text = "Hold",       secs = 4, color = 0xBF5AF2},
  {text = "Breathe out", secs = 6, color = 0x30D158},
}

local arc = ui.arc(0, 100, 0, nil, {align = "center", y = -20, w = 260, h = 260})
local title = ui.label("Ready?", {font = 28, align = "center", y = -20})
local rounds_label = ui.label("", {font = 18, color = 0x8E8E93, align = "bottom", y = -110})
local btn

local phase, elapsed, rounds, tick = 1, 0, 0, nil

local function stop()
  if tick then timer.cancel(tick) end
  tick = nil
  watch.keep_awake(false)
  btn:text("Start")
  title:text("Done")
end

local function step()
  elapsed = elapsed + 0.1
  local p = PHASES[phase]
  local pct = math.floor(elapsed / p.secs * 100)
  if phase == 3 then pct = 100 - pct end          -- the ring shrinks while breathing out
  if phase == 2 then pct = 100 end
  arc:value(pct)
  if elapsed >= p.secs then
    elapsed = 0
    phase = phase % #PHASES + 1
    if phase == 1 then
      rounds = rounds + 1
      rounds_label:text(rounds .. " rounds")
      if rounds >= 5 then stop() return end
    end
    watch.vibrate(20)
    title:text(PHASES[phase].text)
    title:color(PHASES[phase].color)
  end
end

btn = ui.button("Start", function()
  if tick then stop() return end
  phase, elapsed, rounds = 1, 0, 0
  title:text(PHASES[1].text)
  title:color(PHASES[1].color)
  rounds_label:text("")
  watch.keep_awake(true)                          -- don't let the screen sleep mid-exercise
  tick = timer.every(100, step)
  btn:text("Stop")
end, {align = "bottom", y = -30, w = 200})

function on_close()                               -- called when the app closes
  watch.keep_awake(false)
end
