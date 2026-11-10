-- Snake for ESP-Watch OS
-- Controls: the two big buttons turn the snake left / right.
--           Tap the playing field to pause; after game over, tap it to play again.

local CELL, COLS, ROWS = 16, 22, 18          -- 352 x 288 px playing field
local GREEN, HEAD, FOOD = 0x30D158, 0xB4FFC8, 0xFF453A

local best = storage.get("best", 0)
local score_l = ui.label("", {font = 20, align = "top", y = 2})
local field = ui.box({x = 29, y = 30, w = COLS * CELL, h = ROWS * CELL, bg = 0x16161A, radius = 10})
local msg = ui.label("", {parent = field, font = 24, align = "center", text_align = "center"})

-- the snake is a queue of cells {x, y, o}: q[qf] = tail ... q[ql] = head
local q, qf, ql, occ = {}, 1, 0, {}
local dir, turns = {1, 0}, {}
local food, food_o
local score, speed, alive, paused, tick = 0, 170, false, false, nil

local function key(x, y) return x * 100 + y end

local function place(o, x, y) o:pos(x * CELL + 1, y * CELL + 1) end

local function new_cell(color)
  return ui.box({parent = field, w = CELL - 2, h = CELL - 2, radius = 4, bg = color})
end

local function push_head(x, y, o)
  o = o or new_cell(GREEN)
  place(o, x, y)
  ql = ql + 1
  q[ql] = {x = x, y = y, o = o}
  occ[key(x, y)] = true
end

local function show_score()
  score_l:text("Score " .. score .. "     Best " .. best)
end

local function spawn_food()
  local x, y
  repeat
    x, y = math.random(0, COLS - 1), math.random(0, ROWS - 1)
  until not occ[key(x, y)]
  food = {x = x, y = y}
  if not food_o then food_o = new_cell(FOOD) end
  place(food_o, x, y)
end

local function restart_timer()
  if tick then timer.cancel(tick) end
  tick = timer.every(speed, function() step() end)
end

local function die()
  alive = false
  if tick then timer.cancel(tick); tick = nil end
  watch.vibrate(300)
  if score > best then
    best = score
    storage.set("best", best)
    msg:text("New best: " .. score .. "!\n\nTap to play again")
  else
    msg:text("Game over\nScore " .. score .. "\n\nTap to play again")
  end
  show_score()
  watch.keep_awake(false)
end

function step()                                -- global: called by the timer
  if not alive or paused then return end
  if #turns > 0 then dir = table.remove(turns, 1) end
  local h = q[ql]
  local nx, ny = h.x + dir[1], h.y + dir[2]
  if nx < 0 or ny < 0 or nx >= COLS or ny >= ROWS then return die() end   -- wall

  local eat = food and nx == food.x and ny == food.y
  local reuse = nil
  if not eat then                              -- move: the tail block becomes the new head
    local t = q[qf]
    q[qf] = nil
    qf = qf + 1
    occ[key(t.x, t.y)] = nil
    reuse = t.o
  end
  if occ[key(nx, ny)] then return die() end   -- ran into itself

  h.o:bg(GREEN)
  push_head(nx, ny, reuse)
  q[ql].o:bg(HEAD)

  if eat then
    score = score + 1
    watch.vibrate(25)
    show_score()
    spawn_food()
    if score % 5 == 0 and speed > 80 then      -- faster every 5 points
      speed = speed - 15
      restart_timer()
    end
  end
end

local function reset()
  for i = qf, ql do if q[i] then q[i].o:delete() end end
  q, qf, ql, occ = {}, 1, 0, {}
  for i = 0, 3 do push_head(6 + i, 9) end
  q[ql].o:bg(HEAD)
  dir, turns = {1, 0}, {}
  score, speed, alive, paused = 0, 170, true, false
  msg:text("")
  spawn_food()
  show_score()
  watch.keep_awake(true)
  restart_timer()
end

-- relative turns, queued (max 2) so two fast taps work but can't reverse in one step
local function turn(left)
  if not alive or paused then return end
  local base = turns[#turns] or dir
  local nd = left and {base[2], -base[1]} or {-base[2], base[1]}
  if #turns < 2 then table.insert(turns, nd) end
  watch.vibrate(10)
end

field:on_click(function()
  if not alive then return reset() end
  paused = not paused
  msg:text(paused and "Paused\n\nTap to continue" or "")
  watch.keep_awake(not paused)
end)

ui.button(ui.icon("left"), function() turn(true) end,
  {align = "bottom_left", x = 16, y = -12, w = 180, h = 92, font = 36, bg = 0x2C2C2E})
ui.button(ui.icon("right"), function() turn(false) end,
  {align = "bottom_right", x = -16, y = -12, w = 180, h = 92, font = 36, bg = 0x2C2C2E})

show_score()
msg:text("SNAKE\n\nTap to start")

function on_close() watch.keep_awake(false) end
