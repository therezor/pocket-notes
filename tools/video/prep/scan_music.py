"""Tempo + per-bar energy arc for candidate tracks (ffmpeg decode, librosa onset_strength only)."""
import subprocess, sys
import numpy as np, librosa

SR = 22050

def load(path):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.float32)

def tempo(env, hop):
    fps = SR / hop
    best = (0, 0)
    for bpm in np.arange(80, 180, 0.05):
        p = 60 / bpm * fps
        idx = np.arange(0, len(env) - 1, p)
        # comb score over phases
        sc = max(env[np.clip((idx + ph).astype(int), 0, len(env) - 1)].mean() for ph in np.linspace(0, p, 16, endpoint=False))
        if sc > best[0]:
            best = (sc, bpm)
    return best[1]

for path in sys.argv[1:]:
    y = load(path)
    hop = 512
    env = librosa.onset.onset_strength(y=y, sr=SR, hop_length=hop)
    bpm = tempo(env, hop)
    bar = 4 * 60 / bpm
    S = np.abs(librosa.stft(y, n_fft=2048, hop_length=hop))
    f = librosa.fft_frequencies(sr=SR, n_fft=2048)
    lo = (S[f < 120] ** 2).sum(0); full = (S ** 2).sum(0)
    t = np.arange(S.shape[1]) * hop / SR
    nb = int(t[-1] / bar)
    e = [10 * np.log10(full[(t >= i * bar) & (t < (i + 1) * bar)].mean() + 1e-9) for i in range(nb)]
    l = [10 * np.log10(lo[(t >= i * bar) & (t < (i + 1) * bar)].mean() + 1e-9) for i in range(nb)]
    e = np.array(e); l = np.array(l)
    chars = " .:-=+*#%@"
    def strip(a):
        a = (a - a.max() + 18) / 18
        return "".join(chars[int(np.clip(x, 0, 0.999) * len(chars))] for x in a)
    print(f"\n{path.split('/')[-1]}  {len(y)/SR:.1f}s  {bpm:.2f} BPM  bar {bar:.3f}s")
    print(" full |" + strip(e) + "|")
    print(" bass |" + strip(l) + "|")
