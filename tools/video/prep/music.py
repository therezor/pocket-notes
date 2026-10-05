"""Cut the soundtrack: "Intense Electro Trailer Music" (JkStudios, Pixabay 243987, 136 BPM).

Song beats are counted from the first beat at 0.035 s; the drops are on song beats 48 and 208.
The cut plays song beats 42-112 (the last 6 beats of the build, then drop 1), then jumps to song
beat 240, a phrase that starts with the same bass pattern, and runs to the song's own ending and
reverb tail. Both splice points sit 15 ms before the beat so no attack is clipped, with a 30 ms
equal-power crossfade. Output beat 0 is song beat 42; output beat 70 is song beat 240.
Output: public/music.wav (48 kHz stereo).
"""
import numpy as np, soundfile as sf

BPM, OFFSET, LEAD = 136.0, 0.035, 0.015
t = lambda b: OFFSET + b * 60 / BPM
y, sr = sf.read("prep/electro-trailer.wav", dtype="float32")
s0, a, b = (int((t(x) - LEAD) * sr) for x in (42, 112, 240))
xf = int(0.030 * sr)
w = np.linspace(0, np.pi / 2, xf)[:, None]
head, tail = y[s0:a + xf // 2], y[b - xf // 2:]
mix = head[-xf:] * np.cos(w) + tail[:xf] * np.sin(w)
out = np.concatenate([head[:-xf], mix, tail[xf:]])
fade = int(0.010 * sr)
out[:fade] *= np.linspace(0, 1, fade)[:, None]
sf.write("public/music.wav", out, sr, subtype="PCM_24")
print(f"song {s0/sr:.3f}-{a/sr:.3f}s + {b/sr:.3f}s-end, out {len(out)/sr:.3f}s")
