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

Needs: pip install pyserial
"""
import base64
import os
import subprocess
import sys
import time

import serial

PORT = os.environ.get("WATCH_PORT", "/dev/cu.usbmodem1101")


def open_port():
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


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == "shot":
        shot(sys.argv[2] if len(sys.argv) > 2 else "screen.png")
        return
    line = sys.argv[2] if cmd == "raw" else " ".join(sys.argv[1:])
    wait = 8.0 if cmd in ("scan", "raw") else 2.5
    for l in run(line, wait=wait):
        print(l)


if __name__ == "__main__":
    main()
