# Writing apps for ESP-Watch OS

Apps are small **Lua** programs stored on the SD card. You can install them without
reflashing the firmware, and they appear in the launcher next to the built-in apps.

```
/sdcard/apps/
  myapp/
    main.lua          required: the app's code (runs when the app opens)
    manifest.json     optional: name, icon, colour...
    icon.png          optional: 80x80 launcher icon
    utils.lua         optional: extra modules, loaded with require("utils")
    data.json         created by storage.set() - the app's saved data
```

## Quick start (5 minutes)

**Option A: in the browser (no tools needed)**
1. Open the watch's web page (Settings → Web & remote shows the address), then go to the **Apps** tab.
2. Click **New app** and give it a name. It's created, opens in the editor, and starts on the watch.
3. Change the code and press **Ctrl+S**. The app restarts on the watch at once (hot reload).
   `print()` output and errors appear under the editor.

**Option B: on your laptop with your own editor**
```sh
python3 tools/watchctl.py new myapp            # creates ./myapp with a starter app
# edit myapp/main.lua in VS Code ...
python3 tools/watchctl.py app push myapp/ --run    # install over USB and start it
python3 tools/watchctl.py logs                 # see print() output and errors live

# Wi-Fi is much faster than USB for pushing:
WATCH_HOST=192.168.1.50 WATCH_PASS=yourpass python3 tools/watchctl.py app push myapp/ --run
```

Examples to copy from are in [`examples/apps/`](../examples/apps):
- **counter**: buttons and storage
- **breathe**: timers, an animated arc, keeping the screen on
- **todo**: lists, keyboard, choices, saving tables
- **bitcoin**: HTTP and JSON
- **snake**: a complete game - timers, moving boxes, saved best score

To install them all, run `watchctl.py app push examples/apps/todo/` (and the same for the others), or use **Install app folder** in the web Apps tab.

## manifest.json

```json
{
  "name": "My App",
  "icon": "bell",
  "color": "#FF9F0A",
  "version": "1.0",
  "description": "What it does",
  "memory_kb": 512
}
```

- **icon**: a built-in symbol name (list below), or `"icon.png"` (80×80 PNG in the app folder). If missing, the first letter of the name is used.
- **memory_kb**: the app's memory limit, from 128 to 4096. The default is 1024.

## The screen

The app gets the area under the title bar: **410 × 438 pixels** (`ui.W`, `ui.H`). Built-in navigation still works:
- swipe right or press BOOT to go back
- long-press BOOT to go to the watch face

## API reference

### ui: creating things on screen

Every constructor takes an optional **options table** as its last argument and returns an object.

| Function | What it makes |
|---|---|
| `ui.label(text, opts)` | text |
| `ui.button(text, on_click, opts)` | a button; `on_click()` runs when tapped |
| `ui.box(opts)` | an empty container, for grouping or layout (`layout="row"` or `"column"`) |
| `ui.list(opts)` | a full-height scrolling column, the usual parent for `ui.row` |
| `ui.row(text, {value=, icon=, on_click=, parent=})` | a settings-style list row |
| `ui.slider(min, max, value, on_change, opts)` | a slider; `on_change(value)` |
| `ui.switch(on, on_change, opts)` | an on/off switch; `on_change(bool)` |
| `ui.bar(min, max, value, opts)` | a progress bar |
| `ui.arc(min, max, value, on_change, opts)` | a ring; pass `nil` for `on_change` to make it display-only |
| `ui.image("file.png", opts)` | an image from the app folder (PNG, JPG or BMP) |
| `ui.toast(text)` | a short popup message |
| `ui.keyboard(title, initial, function(text) end)` | asks for text |
| `ui.confirm(title, message, function() end [, yes_text])` | yes/cancel; runs the function on yes |
| `ui.choose(title, {"A","B",...}, function(index) end [, current])` | pick one option (index starts at 1) |
| `ui.clear()` | removes everything the app created |
| `ui.icon("play")` | a symbol you can put in any text: `ui.icon("plus") .. " Add"` |
| `ui.accent()` | the user's accent colour |

**Options** (all optional):

| Key | Meaning |
|---|---|
| `x`, `y` | position, or the offset when `align` is set |
| `align` | `"center"`, `"top"`, `"bottom"`, `"left"`, `"right"`, `"top_left"`, `"top_right"`, `"bottom_left"`, `"bottom_right"` |
| `w`, `h` | size in pixels, `"50%"` or `"content"` (setting `w` on a label makes its text wrap) |
| `color` | text colour, e.g. `0xFF453A` |
| `bg` | background colour |
| `font` | 12, 14, 16, 18, 20, 24, 28, 32, 36, 48 |
| `radius`, `pad`, `gap`, `border` (colour), `opacity` (0-255) | styling |
| `text_align` | `"left"`, `"center"`, `"right"` |
| `layout` | `"column"` or `"row"`, for boxes |
| `scroll` | `true` makes a box scrollable |
| `parent` | put the object inside another object (box or list) |

**Object methods** (most return the object, so calls can be chained: `lbl:text("hi"):color(0xFF0000)`):

| Method | |
|---|---|
| `obj:text(s)` / `obj:text()` | set or get text (labels, buttons; rows set their value) |
| `obj:value(v)` / `obj:value()` | sliders, switches, bars, arcs |
| `obj:set(opts)` | apply any options table |
| `obj:color(c)`, `obj:bg(c)` | colours |
| `obj:pos(x, y)`, `obj:size(w, h)`, `obj:align(where, x, y)` | layout |
| `obj:show([bool])`, `obj:hide()` | visibility |
| `obj:on_click(fn)`, `obj:on_change(fn)` | events (any object can be clickable) |
| `obj:clear()` | delete the object's children |
| `obj:delete()`, `obj:exists()` | remove; check if still there |

### timer

```lua
local id = timer.every(1000, function() ... end)   -- repeat (milliseconds, min 20)
timer.after(500, function() ... end)               -- once
timer.cancel(id)
```
An app can have at most 16 timers. They stop automatically when the app closes.

### watch

| | |
|---|---|
| `watch.time()` | `{hour, min, sec, day, month, year, wday (1=Mon), yday, epoch}` |
| `watch.millis()` | milliseconds since boot |
| `watch.battery()` | `{percent, charging, usb, voltage, temp}` |
| `watch.steps()` | steps today |
| `watch.weather()` | `{valid, temp, feels, high, low, humidity, text}` |
| `watch.wifi()` | `{connected, ssid, ip, rssi}` |
| `watch.vibrate(ms)` | vibration, max 1000 ms |
| `watch.beep(hz, ms)` | short tone at the user's volume |
| `watch.keep_awake(bool)` | keep the screen on while the app is open |

### storage: saved on the SD card, in the app's own folder

```lua
storage.set("best", 42)            -- numbers, strings, booleans and tables
local best = storage.get("best", 0) -- 0 = default if not saved yet
storage.set("best", nil)           -- delete
storage.write("notes.txt", text)   -- whole files (inside the app folder only)
storage.append("log.csv", "1,2\n")
local t = storage.read("notes.txt") -- nil if missing
storage.remove("notes.txt")
```

### http and json: needs Wi-Fi with internet

```lua
http.get("https://api.example.com/x", function(status, body)
  local data = json.decode(body)    -- returns nil if it isn't JSON
end)
http.post(url, json.encode({a = 1}), function(status, body) end)   -- optional 4th arg: content type
```
Requests run in the background, and the function is called when the answer arrives. Responses are limited to 32 KB, with at most 3 requests at once.

### app, print, require, os

| | |
|---|---|
| `app.id`, `app.name`, `app.version` | from the manifest |
| `app.exit()` | close the app |
| `app.memory()` | `used_kb, limit_kb` |
| `function on_close() ... end` | define this global and it runs when the app closes |
| `print(...)` | writes to the log (`watchctl.py logs`, web Apps editor, `nc watch 23` → `log`) |
| `require("utils")` | loads `utils.lua` from the app folder |
| `os.time()`, `os.date(fmt)`, `os.clock()` | time helpers (`os.date("%H:%M")`) |

Standard Lua is available: `string`, `table`, `math`, `utf8`, `coroutine`, `pairs`, `pcall`, etc.

### Icon names

audio, video, list, ok, close, power, settings, home, download, drive, refresh, mute, volume,
image, edit, prev, play, pause, stop, next, eject, left, right, plus, minus, eye, warning,
shuffle, up, down, loop, folder, upload, call, cut, copy, save, bars, mail, charge, paste,
bell, keyboard, gps, file, wifi, battery, bluetooth, trash, backspace, sd

## Safety rules (the sandbox)

Apps can't break the watch:
- **No file access outside the app's folder**, and no `io`, `os.execute`, `debug` or loading of compiled bytecode.
- **Memory limit** from `memory_kb`. An app that uses more gets a "not enough memory" error.
- **Time limit:** any single piece of code (the start-up or one callback) may run at most **1.5 s**. An endless loop stops the app with an error instead of freezing the watch.
- **Errors** stop only the app. The error and the line number are shown on the watch with a **Restart app** button, and they're printed to the log.
- Only one Lua app runs at a time. It stops when you leave it, so apps don't drain the battery in the background.

## Tips

- Keep timers slow (≥ 100 ms for animations, ≥ 1 s for clocks). Every redraw costs battery.
- Draw the screen once, then update with `obj:text()` and `obj:value()` instead of `ui.clear()` and rebuilding.
- Use `print()` freely while developing. It goes to the log, not the screen.
- **Uninstall:** long-press the app in the launcher, use the web Apps tab, or run `watchctl.py app rm myapp`.
