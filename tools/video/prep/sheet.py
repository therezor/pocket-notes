"""Contact sheet of a preview render at given beats: python prep/sheet.py in.mp4 out.png beat beat ..."""
import re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
src, out, beats = sys.argv[1], sys.argv[2], [float(b) for b in sys.argv[3:]]
consts = dict(re.findall(r"export const (BPM|OFFSET_S) = ([0-9.]+);", open("src/beats.ts").read()))
BPM, OFF = float(consts["BPM"]), float(consts["OFFSET_S"])
w, h = map(int, subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=width,height", "-of", "csv=p=0", src], capture_output=True, text=True).stdout.strip().strip(",").split(",")[:2])
raw = subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
v = np.frombuffer(raw, np.uint8).reshape(-1, h, w, 3)
cols = 4
rows = (len(beats) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (w + 4), rows * (h + 22)), "black")
d = ImageDraw.Draw(sheet)
for i, b in enumerate(beats):
    f = min(len(v) - 1, round((OFF + b * 60 / BPM) * 30))
    x, y = (i % cols) * (w + 4), (i // cols) * (h + 22)
    sheet.paste(Image.fromarray(v[f]), (x, y + 20))
    d.text((x + 4, y + 4), f"beat {b}  f{f}", fill="yellow")
sheet.save(out)
