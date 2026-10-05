#!/usr/bin/env python3
"""Drive a Cardputer running Pocket Notes over USB serial: inject keys, grab screenshots, dump notes.

    ~/.platformio/penv/bin/python tools/remote.py "key down down enter" "shot list" dump
    ~/.platformio/penv/bin/python tools/remote.py --script tools/smoke.txt

Each argument (or script line) is one device command (see firmware/main/remote.h), plus:
    wait <ms>        sleep on the host
    expect <text>    fail unless the last dump contains <text>
    reject <text>    fail if the last dump contains <text>
Screenshots go to --out (default docs/img) as PNG. Pure standard library + pyserial.
"""
import argparse
import glob
import os
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial not found - run with ~/.platformio/penv/bin/python")


def find_port():
    for pattern in ("/dev/cu.usbmodem*", "/dev/ttyACM*"):
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    sys.exit("no serial port found - pass --port")


def write_png(path, w, h, payload, scale):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            i = (y * w + x) * 2
            v = (payload[i] << 8) | payload[i + 1]       # canvas pixels are MSB first
            r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * scale
        rows.extend([bytes(row)] * scale)

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w * scale, h * scale, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def read_line(ser, deadline):
    out = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            return out.decode("utf-8", "replace").rstrip("\r")
        out += b
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*")
    ap.add_argument("--port")
    ap.add_argument("--script")
    ap.add_argument("--out", default="docs/img")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--log", action="store_true", help="echo device log lines")
    args = ap.parse_args()
    cmds = list(args.commands)
    if args.script:
        with open(args.script) as f:
            cmds += [l.strip() for l in f if l.strip() and not l.lstrip().startswith("#")]

    ser = serial.Serial()
    ser.port = args.port or find_port()
    ser.baudrate = 115200
    ser.timeout = 0.1
    ser.rts = False   # RTS high with DTR low resets the ESP32-S3; keep the lines where they are
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()
    last_dump = ""
    failures = 0
    for c in cmds:
        verb, _, arg = c.partition(" ")
        if verb == "wait":
            time.sleep(int(arg) / 1000)
            continue
        if verb in ("expect", "reject"):
            hit = arg in last_dump
            if hit != (verb == "expect"):
                failures += 1
                print(f"FAIL {c}")
            else:
                print(f"ok   {c}")
            continue
        ser.write((c + "\n").encode())
        ser.flush()
        if verb == "key" or verb == "type":
            time.sleep(0.15 + 0.03 * len(arg.split() if verb == "key" else arg))
            continue
        deadline = time.time() + 10
        if verb == "shot":
            while True:
                line = read_line(ser, deadline)
                if line is None:
                    print("! no screenshot"); failures += 1; break
                if line.startswith("<<SHOT "):
                    _, name, w, h = line.split()
                    w, h = int(w), int(h)
                    payload = bytearray()
                    while len(payload) < w * h * 2 and time.time() < deadline:
                        payload += ser.read(w * h * 2 - len(payload))
                    os.makedirs(args.out, exist_ok=True)
                    path = os.path.join(args.out, name + ".png")
                    write_png(path, w, h, payload, args.scale)
                    print(f"shot {path}")
                    break
                elif args.log:
                    print("  " + line)
        else:
            out = []
            while True:
                line = read_line(ser, deadline)
                if line is None:
                    break
                if line.startswith("##"):
                    out.append(line[2:].strip())
                    if verb != "dump" or line.startswith("## end"):
                        break
                elif args.log:
                    print("  " + line)
            text = "\n".join(out)
            if verb == "dump":
                last_dump = text
            print(text)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
