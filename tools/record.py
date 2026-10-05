#!/usr/bin/env python3
"""Record a demo video from the Cardputer's own screen while a script drives the app.

    ~/.platformio/penv/bin/python tools/record.py tools/demo.txt docs/media/demo

Frames are canvas captures streamed over USB (tools/remote.py's "shot" command), so the video is
the real firmware at device speed. Writes <out>.mp4 (720p, framed) and <out>.gif (README size).

Script lines (see tools/demo.txt):
    seed <device command>     run before recording starts (not filmed)
    key <names> / type <text> as in remote.py; "type" is filmed one character at a time
    hold <ms>                 keep filming without input
    # ...                     comment
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from remote import find_port, read_line, write_png  # noqa: E402

import serial  # noqa: E402

TYPE_MS = 110          # pause after each typed character
KEY_MS = 450           # pause after a navigation key


def open_port():
    ser = serial.Serial()
    ser.port = find_port()
    ser.baudrate = 115200
    ser.timeout = 0.1
    ser.rts = False    # RTS high with DTR low resets the board
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def grab(ser):
    ser.write(b"shot f\n")
    ser.flush()
    deadline = time.time() + 5
    while True:
        line = read_line(ser, deadline)
        if line is None:
            return None
        if line.startswith("<<SHOT "):
            _, _, w, h = line.split()
            w, h = int(w), int(h)
            buf = bytearray()
            while len(buf) < w * h * 2 and time.time() < deadline:
                buf += ser.read(w * h * 2 - len(buf))
            return (w, h, bytes(buf)) if len(buf) == w * h * 2 else None


class Recorder:
    def __init__(self, ser, tmp):
        self.ser, self.tmp, self.frames = ser, tmp, []   # (path, t)

    def film(self, ms):
        end = time.time() + ms / 1000
        while True:
            f = grab(self.ser)
            if f:
                path = os.path.join(self.tmp, f"f{len(self.frames):05d}.png")
                write_png(path, f[0], f[1], f[2], 1)
                self.frames.append((path, time.time()))
            if time.time() >= end:
                break

    def send(self, cmd):
        self.ser.write((cmd + "\n").encode())
        self.ser.flush()


CARD_SVG = """<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="720">
  <style>text {{ font-family: Helvetica, Arial, sans-serif; }}</style>
  <text x="280" y="112" font-size="30" font-weight="bold" fill="#38bdf8">POCKET NOTES</text>
  <text x="1000" y="112" font-size="22" fill="#a8b6c8" text-anchor="end">notes sorted by an on-device AI</text>
  <text x="640" y="620" font-size="20" fill="#6b7280" text-anchor="middle">{caption}</text>
</svg>"""


def encode(concat, out, tmp):
    """Frames (ffmpeg concat list) -> framed 720p MP4 + bare-screen GIF."""
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    svg, card = os.path.join(tmp, "card.svg"), os.path.join(tmp, "card.png")
    with open(svg, "w") as f:
        f.write(CARD_SVG.format(caption="real device capture \u00b7 Cardputer ADV \u00b7 device speed"))
    subprocess.run(["rsvg-convert", "-o", card, svg], check=True)
    # 240x135 -> 3x nearest-neighbour (crisp pixels), centred on a 1280x720 dark card, caption on top.
    graph = ("[0:v]scale=720:405:flags=neighbor,pad=1280:720:280:150:color=0x14171d[s];"
             "[s][1:v]overlay=0:0:shortest=1,fps=30,format=yuv420p")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", concat,
                    "-loop", "1", "-i", card, "-filter_complex", graph, "-shortest",
                    "-c:v", "libx264", "-crf", "18", "-movflags", "+faststart", out + ".mp4"], check=True)
    # README GIF: the bare screen at 2x, 12 fps, own palette.
    pal = os.path.join(tmp, "pal.png")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", concat,
                    "-vf", "fps=12,scale=480:270:flags=neighbor,palettegen=max_colors=64", pal], check=True)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", concat, "-i", pal,
                    "-lavfi", "fps=12,scale=480:270:flags=neighbor[x];[x][1:v]paletteuse=dither=none",
                    out + ".gif"], check=True)
    print(f"wrote {out}.mp4 and {out}.gif")


def main():
    if sys.argv[1] == "--encode":            # re-encode a kept recording: --encode <frames dir> <out>
        d = sys.argv[2]
        encode(os.path.join(d, "list.txt"), sys.argv[3], d)
        return
    script, out = sys.argv[1], sys.argv[2]
    lines = [l.rstrip("\n") for l in open(script) if l.strip() and not l.lstrip().startswith("#")]
    ser = open_port()
    tmp = tempfile.mkdtemp(prefix="pn_rec_")
    rec = Recorder(ser, tmp)

    for l in lines:                                   # setup, not filmed
        if l.startswith("seed "):
            cmd = l[5:]
            if cmd.startswith("wait "):
                time.sleep(int(cmd[5:]) / 1000)
            else:
                rec.send(cmd)
                time.sleep(0.12 + 0.02 * len(cmd))
    ser.reset_input_buffer()

    rec.film(1200)
    for l in lines:
        verb, _, arg = l.partition(" ")
        if verb == "seed":
            continue
        if verb == "hold":
            rec.film(int(arg))
        elif verb == "type":
            for c in arg:
                rec.send("key space" if c == " " else f"key {c}")
                rec.film(TYPE_MS)
        elif verb == "key":
            for k in arg.split():
                rec.send(f"key {k}")
                rec.film(KEY_MS)
        else:
            rec.send(l)
            rec.film(300)
    rec.film(1000)
    print(f"{len(rec.frames)} frames over {rec.frames[-1][1] - rec.frames[0][1]:.1f} s")

    # Real timing: each frame lasts until the next one was captured.
    concat = os.path.join(tmp, "list.txt")
    with open(concat, "w") as f:
        for i, (p, t) in enumerate(rec.frames):
            d = (rec.frames[i + 1][1] - t) if i + 1 < len(rec.frames) else 1.0
            f.write(f"file '{p}'\nduration {d:.3f}\n")
        f.write(f"file '{rec.frames[-1][0]}'\n")

    encode(concat, out, tmp)
    shutil.rmtree(tmp)


if __name__ == "__main__":
    main()
