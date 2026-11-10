# Feature checklist

✅ done · 🧪 built, needs on-device test · ❌ not possible on this hardware · ⏳ later

## Settings
| Feature | State |
|---|---|
| Wi-Fi on/off, scan, join (keyboard), 5 saved networks, forget, details (IP/gateway/DNS/MAC/RSSI) | 🧪 |
| Hotspot (name, password, clients, QR code to join) | 🧪 |
| Bluetooth on/off, name, status, address, Gadgetbridge help | 🧪 |
| Web & remote: on/off, address + QR, password | 🧪 |
| Display: brightness, timeout, raise to wake, always-on, watch face, accent colour | ✅ (page tested) |
| Sound: volume, mute, vibration, touch feedback, test buttons | 🧪 |
| Notifications: on/off, wake screen, DND, DND schedule | 🧪 |
| Date & time: automatic (NTP + phone), manual time/date, time zone, 12/24 h | 🧪 |
| Battery: %, state, voltage, temperature, power saving, restart, power off | 🧪 |
| Storage: SD usage, unmount/mount, format, internal flash/PSRAM | 🧪 |
| Weather location, activity goal, move reminder, updates, about, factory reset | 🧪 |

## Watch & system
| Feature | State |
|---|---|
| Digital / analog / minimal faces, always-on dim mode | ✅ digital tested |
| Gestures, launcher, quick settings, notification centre, widgets | ✅ launcher tested |
| Screen timeout, wake by touch / PWR / raise, power menu | 🧪 |
| Power management (80 MHz screen off), Wi-Fi modem sleep, 300 mA charging | 🧪 |
| Low-battery warnings, hourly move reminder | 🧪 |

## Phone (Android, Gadgetbridge)
| Feature | State |
|---|---|
| Notifications (vibrate, banner, list, dismiss) | 🧪 |
| Incoming call: accept / reject / silence; missed-call notification | 🧪 |
| Talking on the watch | ❌ needs Classic Bluetooth |
| Phone music remote, find my phone, time + time-zone sync, weather | 🧪 |

## Apps
| App | State |
|---|---|
| Files (copy/cut/paste/rename/delete/info), text viewer | 🧪 |
| Music MP3/WAV (library, now playing, shuffle, background) | 🧪 |
| Gallery JPG/PNG/BMP | 🧪 |
| Video MJPEG/AVI (no sound); MP4 → convert | 🧪 |
| Recorder (WAV, level meter, play, delete) | 🧪 |
| Alarms, timer, stopwatch | 🧪 |
| Activity, weather, flashlight, find phone, calculator | 🧪 |
| Streaming phone audio / Bluetooth earbuds | ❌ needs Classic Bluetooth |

## Remote & updates
| Feature | State |
|---|---|
| Web dashboard: status, live screen + tap/swipe, files, settings, alarms, notifications, updates | 🧪 |
| OTA from GitHub Releases (private repo token), browser upload, A/B rollback | 🧪 |
| Remote management from outside home Wi-Fi (internet relay) | ⏳ chosen for later |
| USB control tool (watchctl.py): status, screenshot, tap, swipe, settings, time | ✅ |
| Steps kept across restarts, 14-day history, CSV logs (daily / hourly steps, battery) on SD, 7-day chart | 🧪 |
| PIN lock, sleep tracking | ⏳ |

## Apps & developers
| Feature | State |
|---|---|
| Lua app runtime: /sdcard/apps, sandbox (memory + time limit), errors shown on watch | 🧪 (examples tested on laptop mock) |
| Lua API: ui (labels, buttons, lists, sliders, switches, arcs, images, dialogs), timer, watch, storage, http, json | 🧪 |
| Install / hot reload: web Apps tab with editor, folder upload, `watchctl app push`, USB `putb` | 🧪 |
| Launcher shows installed apps; long-press to uninstall | 🧪 |
| Network console (port 23, password), live log follow | 🧪 |
| Log ring, crash dumps, task monitor, web console, FPS overlay | 🧪 |
