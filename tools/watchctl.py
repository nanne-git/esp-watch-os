#!/usr/bin/env python3
"""
Control ESP-Watch OS over the USB cable.

  watchctl.py status                      battery / Wi-Fi / memory
  watchctl.py shot screen.png             screenshot (PNG via macOS 'sips', else BMP)
  watchctl.py tap 205 250                 touch the screen
  watchctl.py swipe 205 450 205 100       swipe (here: bottom -> top = open apps)
  watchctl.py key back|home|power|wake
  watchctl.py scan | join SSID PASS | set brightness 80 | ls / | help
  watchctl.py raw "any command"

Lua apps (see docs/APPS.md):
  watchctl.py new myapp                   create a starter app folder ./myapp
  watchctl.py app push myapp/ [--run]     install/update the app on the watch (then start it)
  watchctl.py apps                        list installed apps
  watchctl.py app run|rm myapp            start (hot reload) / uninstall
  watchctl.py app stop
  watchctl.py logs                        live log (print() output of apps) - Ctrl+C to stop

  Over Wi-Fi instead of USB (much faster): WATCH_HOST=192.168.1.50 [WATCH_PASS=...] watchctl.py app push myapp/

Needs: pip install pyserial
"""
import base64
import json
import os
import subprocess
import sys
import time
import urllib.parse
import urllib.request


PORT = os.environ.get("WATCH_PORT", "/dev/cu.usbmodem1101")


def open_port():
    try:
        import serial
    except ImportError:
        sys.exit("needs pyserial: pip3 install pyserial  (or use WATCH_HOST=<ip> for Wi-Fi)")
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True           # DTR high + RTS low: opening the port does not reset the chip
    s.rts = False
    s.open()
    return s


def run(cmd, wait=2.0, quiet_after=0.6):
    """Send one command line, return the reply lines (log lines filtered out)."""
    s = open_port()
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    out = b""
    end = time.time() + wait
    last = time.time()
    while time.time() < end:
        chunk = s.read(65536)
        if chunk:
            out += chunk
            last = time.time()
        elif out and time.time() - last > quiet_after:
            break
    s.close()
    lines = out.decode(errors="replace").splitlines()
    return [l for l in lines if not l.startswith(("I (", "W (", "E (", "D (")) and l.strip()]


def shot(path):
    s = open_port()
    s.reset_input_buffer()
    s.write(b"shot\n")
    data = b""
    end = time.time() + 30
    while time.time() < end and b"SHOT_END" not in data:
        data += s.read(65536)
    s.close()
    text = data.decode(errors="replace")
    if "SHOT_RLE" not in text or "SHOT_END" not in text:
        sys.exit("no screenshot received")
    head, body = text.split("SHOT_RLE", 1)[1].split("\n", 1)
    bmp_len, hdr_len = (int(v) for v in head.split())
    body = body.split("SHOT_END", 1)[0]
    b64 = "".join(l.strip() for l in body.splitlines()
                  if l.strip() and not l.startswith(("I (", "W (", "E (", "D (")))
    rle = base64.b64decode(b64)
    out = bytearray(rle[:hdr_len])
    i = hdr_len
    while i + 2 < len(rle) + 1 and len(out) < bmp_len:
        run, lo, hi = rle[i], rle[i + 1], rle[i + 2]
        out += bytes((lo, hi)) * run
        i += 3
    base, ext = os.path.splitext(path)
    bmp_path = base + ".bmp"
    with open(bmp_path, "wb") as f:
        f.write(bytes(out[:bmp_len]))
    if ext.lower() == ".png":
        r = subprocess.run(["sips", "-s", "format", "png", bmp_path, "--out", path], capture_output=True)
        if r.returncode == 0:
            os.remove(bmp_path)
            print(path)
            return
    print(bmp_path)


# ---------------------------------------------------------------- Lua apps

TEMPLATE_MAIN = """-- {name}: a starter app for ESP-Watch OS. API: docs/APPS.md
local count = storage.get("count", 0)

ui.label("{name}", {{font = 32, align = "top", y = 10}})
local label = ui.label("Taps: " .. count, {{font = 48, align = "center", y = -30}})

ui.button("Tap me", function()
  count = count + 1
  label:text("Taps: " .. count)
  storage.set("count", count)   -- saved on the SD card, survives restarts
  watch.vibrate(30)
end, {{align = "bottom", y = -40}})

print("{name} started")          -- shows up in: watchctl.py logs
"""


def cmd_new(app_id):
    if os.path.exists(app_id):
        sys.exit(f"{app_id} already exists")
    os.makedirs(app_id)
    name = app_id.replace("_", " ").replace("-", " ").title()
    with open(os.path.join(app_id, "manifest.json"), "w") as f:
        json.dump({"name": name, "icon": "edit", "color": "#5E5CE6", "version": "1.0",
                   "description": "My first watch app", "memory_kb": 512}, f, indent=2)
        f.write("\n")
    with open(os.path.join(app_id, "main.lua"), "w") as f:
        f.write(TEMPLATE_MAIN.format(name=name))
    print(f"created {app_id}/  ->  edit {app_id}/main.lua, then: watchctl.py app push {app_id}/ --run")


def app_files(folder):
    out = []
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for fn in files:
            if fn.startswith(".") or fn == "data.json":     # never overwrite the app's saved data
                continue
            full = os.path.join(root, fn)
            out.append((os.path.relpath(full, folder).replace(os.sep, "/"), full))
    return out


def http(method, path, data=None):
    host = os.environ["WATCH_HOST"]
    req = urllib.request.Request(f"http://{host}{path}", data=data, method=method)
    pw = os.environ.get("WATCH_PASS")
    if pw:
        req.add_header("Authorization", "Basic " + base64.b64encode(f"admin:{pw}".encode()).decode())
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode(errors="replace")


def push_usb(app_id, files):
    s = open_port()
    s.reset_input_buffer()

    def send(line):
        s.write((line + "\n").encode())
        buf = b""
        end = time.time() + 5
        while time.time() < end:
            buf += s.read(4096)
            for l in buf.decode(errors="replace").splitlines():
                if l.startswith("OK"):
                    return
                if l.startswith("ERR"):
                    sys.exit(f"watch: {l}")
        sys.exit("no answer from the watch (is it connected?)")

    for rel, full in files:
        data = open(full, "rb").read()
        path = f"/apps/{app_id}/{rel}"
        room = 240 - len(f"putb w {path} ")          # the watch reads lines of max 244 chars
        if room < 8:
            sys.exit(f"path too long for USB transfer: {path}")
        chunk = room // 4 * 3                        # base64 makes 3 bytes into 4 chars
        parts = [data[i:i + chunk] for i in range(0, len(data), chunk)] or [b""]
        for i, part in enumerate(parts):
            send(f"putb {'w' if i == 0 else 'a'} {path} {base64.b64encode(part).decode()}")
        print(f"  {rel} ({len(data)} bytes)")
    s.close()


def push_http(app_id, files):
    for d in ["/apps", f"/apps/{app_id}"] + sorted({f"/apps/{app_id}/" + os.path.dirname(r) for r, _ in files if "/" in r}):
        try:
            http("POST", "/api/mkdir?path=" + urllib.parse.quote(d))
        except Exception:
            pass                                   # already exists
    for rel, full in files:
        q = urllib.parse.quote(f"/apps/{app_id}/{rel}")
        http("POST", f"/api/upload?overwrite=1&path={q}", open(full, "rb").read())
        print(f"  {rel} ({os.path.getsize(full)} bytes)")


def cmd_app_push(folder, run_after):
    folder = folder.rstrip("/")
    app_id = os.path.basename(os.path.abspath(folder))
    if not os.path.exists(os.path.join(folder, "main.lua")):
        sys.exit(f"{folder}/main.lua not found")
    files = app_files(folder)
    print(f"installing {app_id} ({len(files)} files) via {'Wi-Fi' if 'WATCH_HOST' in os.environ else 'USB'}")
    (push_http if "WATCH_HOST" in os.environ else push_usb)(app_id, files)
    if run_after:
        app_cmd(["run", app_id])
    print("done")


def app_cmd(args):
    if "WATCH_HOST" in os.environ:
        if args[0] == "run":
            print(http("POST", "/api/apps/run?id=" + urllib.parse.quote(args[1])))
        elif args[0] == "stop":
            print(http("POST", "/api/apps/stop"))
        elif args[0] == "rm":
            print(http("POST", "/api/apps/delete?id=" + urllib.parse.quote(args[1])))
        return
    for l in run("app " + " ".join(args)):
        print(l)


def cmd_apps():
    if "WATCH_HOST" in os.environ:
        d = json.loads(http("GET", "/api/apps"))
        for a in d["apps"]:
            mark = "*" if a["id"] == d["running"] else " "
            print(f"{mark} {a['id']:<16} {a['name']:<20} v{a['version']}")
        return
    for l in run("apps"):
        print(l)


def cmd_logs():
    s = open_port()
    print("live log - Ctrl+C to stop")
    buf = b""
    try:
        while True:
            buf += s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(line.decode(errors="replace").rstrip())
    except KeyboardInterrupt:
        pass
    s.close()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "new" and len(sys.argv) > 2:
        cmd_new(sys.argv[2])
        return
    if cmd == "apps":
        cmd_apps()
        return
    if cmd == "logs":
        cmd_logs()
        return
    if cmd == "app" and len(sys.argv) > 3 and sys.argv[2] == "push":
        cmd_app_push(sys.argv[3], "--run" in sys.argv)
        return
    if cmd == "app" and len(sys.argv) > 2:
        app_cmd(sys.argv[2:])
        return
    if cmd == "shot":
        shot(sys.argv[2] if len(sys.argv) > 2 else "screen.png")
        return
    line = sys.argv[2] if cmd == "raw" else " ".join(sys.argv[1:])
    wait = 8.0 if cmd in ("scan", "raw") else 2.5
    for l in run(line, wait=wait):
        print(l)


if __name__ == "__main__":
    main()
