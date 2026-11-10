# ESP-Watch OS

Custom smartwatch firmware for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**
(ESP32-S3, 410×502 AMOLED touch screen, 32 MB flash, 8 MB PSRAM, BLE 5, Wi-Fi).

Built with ESP-IDF 6.1, LVGL 9.5 and Waveshare's board support package.

## What it does

| Area | Features |
|---|---|
| Watch faces | Digital (steps / notifications / battery rings), Analog, Minimal; always-on dim mode; long-press to switch |
| Navigation | Swipe **up** = apps, **down** = quick settings, **left** = notifications, **right** = today widgets; swipe right or BOOT = back; BOOT long = home; PWR = screen on/off, PWR long = power menu |
| Settings | Wi-Fi (scan, join, 5 saved networks, hotspot, details), Bluetooth, Web & remote, Display, Sound & vibration, Notifications + Do Not Disturb schedule, Date & time / time zone, Battery & power saving, Storage (unmount / format), Weather location, Activity goal, Updates, About + factory reset |
| Phone (Android) | Notifications, incoming-call alerts with accept / reject, phone music remote, find my phone, time sync — via **Gadgetbridge** |
| Apps | Files, Music (MP3/WAV), Gallery (JPG/PNG/BMP), Video (MJPEG), Voice recorder, Alarms, Timer, Stopwatch, Activity (steps), Weather, Flashlight, Find phone, Calculator |
| Remote management | Web dashboard on the watch: status, **live screen you can tap/swipe**, files, settings editor, alarms, send notifications, firmware updates; optional password |
| Updates | Over the internet from **GitHub Releases** (A/B slots with automatic rollback) or by uploading a `.bin` in the browser |

Not possible on this chip (BLE only, no Classic Bluetooth): talking on calls through the watch,
streaming phone audio to the watch, Bluetooth earbuds. MP4/H.264 video must be converted to MJPEG.

## Build and flash

```bash
. ~/.espressif/v6.1/esp-idf/export.sh
cd ~/esp-projects/esp-watch-os
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor      # Ctrl+] leaves the monitor
```

The default Wi-Fi to join on first boot is set in `idf.py menuconfig` → *ESP-Watch settings*.
After that, add networks on the watch (Settings → Wi-Fi → Find networks).

## Connect your Android phone

1. Install **Gadgetbridge** (F-Droid or Play Store).
2. In Gadgetbridge tap **+**, choose **Bangle.js ESP-Watch**.
3. Allow notification access for Gadgetbridge.

Keep the Bluetooth name starting with `Bangle.js` — that is how Gadgetbridge recognises the watch.

## Web dashboard (remote management)

Open the address shown in Settings → Web & remote (e.g. `http://192.168.1.50/` or
`http://esp-watch.local/`) from a browser on the same Wi-Fi, or join the watch hotspot
(`ESP-Watch` / `watchfiles`) and open `http://192.168.4.1/`.

Set a password under Settings → Web & remote → Password (browser asks for it; any user name).

## Updates from anywhere

```bash
./tools/release.sh 1.1.0 "What changed"
```

This tags the commit, builds it and publishes `esp-watch-os.bin` as a GitHub release.
The watch checks every 6 hours (or Settings → Updates → Check), downloads over any
internet connection and restarts. If the new firmware fails to start, the bootloader
returns to the previous one.

The repository is private, so the watch needs a **read-only token**:
GitHub → Settings → Developer settings → Fine-grained tokens → *Only this repository*,
permission *Contents: Read-only*. Enter it in Settings → Updates → Access token
(or in the web dashboard's Settings tab).

## Control from the laptop (USB)

```bash
pip install pyserial
python3 tools/watchctl.py status
python3 tools/watchctl.py shot screen.png          # screenshot
python3 tools/watchctl.py tap 205 250
python3 tools/watchctl.py swipe 205 460 205 120    # open apps
python3 tools/watchctl.py key back
python3 tools/watchctl.py join "MyWiFi" "password"
python3 tools/watchctl.py set volume 20
python3 tools/watchctl.py time $(date +%s)
```

The same text commands (plus file commands like `ls`, `cp`, `rm`) also work over BLE (Nordic UART).

## Make your own apps (Lua)

Apps are Lua scripts on the SD card (`/apps/<name>/main.lua`), installed and hot-reloaded without
reflashing. Create one in the web dashboard's **Apps** tab (built-in editor, Ctrl+S = run on the
watch) or from the laptop:

```sh
python3 tools/watchctl.py new myapp && python3 tools/watchctl.py app push myapp/ --run
```

Full guide and API: [docs/APPS.md](docs/APPS.md). Examples: [examples/apps/](examples/apps).

## History

Steps survive restarts (saved in flash), and the watch writes CSV files you can open in Excel to
`/sdcard/History/`: `daily.csv`, `steps-YYYY-MM.csv` (hourly) and `battery-YYYY-MM.csv`
(every 10 minutes). The Activity app shows the last 7 days.

## Network console

`nc esp-watch.local 23` (or the watch's IP address) gives the same command console as USB, over
Wi-Fi. It asks for the web password. Type `log` to follow the live log, and `exit` to leave.

## Restore the original Waveshare firmware

```bash
. ~/.espressif/v6.1/esp-idf/export.sh
esptool --port /dev/cu.usbmodem1101 write-flash 0 ~/esp-watch-backup/waveshare_amoled_2_06_original_32MB.bin
```

## Project layout

```
main/
  main.c            start-up order
  hw/               power chip, RTC, motion sensor, vibration, buttons
  svc/              settings, Wi-Fi, BLE + Gadgetbridge, time, alarms, audio,
                    weather, SD file manager, web server/API, OTA
  ui/               LVGL framework (pages, gestures, sleep, overlays), widgets, home screen
  apps/             every app in the launcher
  www/index.html    web dashboard (embedded in the firmware)
tools/              watchctl.py (USB control), release.sh (publish an update)
docs/FEATURES.md    feature checklist
```
