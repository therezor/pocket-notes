"""Per-beat energy grid (rows = 4 bars) for one track: full band and <120 Hz."""
import subprocess, sys
import numpy as np, librosa
SR = 22050
path, bpm, off = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", "-"], capture_output=True, check=True).stdout
y = np.frombuffer(raw, np.float32)
hop = 256
S = np.abs(librosa.stft(y, n_fft=2048, hop_length=hop)) ** 2
f = librosa.fft_frequencies(sr=SR, n_fft=2048)
t = np.arange(S.shape[1]) * hop / SR
full, lo, hi = S.sum(0), S[f < 120].sum(0), S[f > 4000].sum(0)
beat = 60 / bpm
nb = int((t[-1] - off) / beat)
def db(a, i): m = (t >= off + i * beat) & (t < off + (i + 1) * beat); return 10 * np.log10(a[m].mean() + 1e-12)
F = np.array([db(full, i) for i in range(nb)]); L = np.array([db(lo, i) for i in range(nb)]); H = np.array([db(hi, i) for i in range(nb)])
ch = " .:-=+*#%@"
g = lambda a, x: ch[int(np.clip((x - a.max() + 20) / 20, 0, .999) * 10)]
for r in range(0, nb, 16):
    s = " ".join("".join(g(A, A[i]) for i in range(r, min(r + 16, nb))) for A in (F, L, H))
    print(f"b{r:4d} {off + r * beat:6.2f}s  full {''.join(g(F,F[i]) for i in range(r,min(r+16,nb)))}  bass {''.join(g(L,L[i]) for i in range(r,min(r+16,nb)))}  hi {''.join(g(H,H[i]) for i in range(r,min(r+16,nb)))}")
