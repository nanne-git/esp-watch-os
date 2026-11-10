-- Counter: shows labels, buttons, storage and vibration.
local count = storage.get("count", 0)

local label = ui.label(tostring(count), {font = 48, align = "center", y = -60})

local function show()
  label:text(tostring(count))
  storage.set("count", count)        -- kept in /apps/counter/data.json
end

ui.button(ui.icon("plus") .. " Add", function()
  count = count + 1
  watch.vibrate(25)
  show()
end, {align = "center", y = 40, w = 220})

ui.button("Reset", function()
  ui.confirm("Reset", "Set the counter back to 0?", function()
    count = 0
    show()
  end)
end, {align = "bottom", y = -30, bg = 0x3A3A3C})
