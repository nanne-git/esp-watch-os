-- Bitcoin: http.get + json.decode. Needs Wi-Fi with internet.
ui.label("BTC / USD", {font = 24, color = 0x8E8E93, align = "top", y = 30})
local price = ui.label("...", {font = 48, align = "center", y = -40})
local status = ui.label("", {font = 16, color = 0x8E8E93, align = "center", y = 30})

local function refresh()
  if not watch.wifi().connected then
    status:text("No Wi-Fi")
    return
  end
  status:text("Loading...")
  http.get("https://api.coinbase.com/v2/prices/BTC-USD/spot", function(code, body)
    local data = code == 200 and json.decode(body)
    if data and data.data then
      price:text("$" .. string.format("%.0f", tonumber(data.data.amount)))
      status:text("Updated " .. os.date("%H:%M"))
    else
      status:text("Error " .. tostring(code))
    end
  end)
end

ui.button(ui.icon("refresh") .. " Refresh", refresh, {align = "bottom", y = -30})
refresh()
timer.every(60000, refresh)                       -- every minute while the app is open
