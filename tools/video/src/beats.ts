// Beat grid of the soundtrack (public/music.wav, cut by prep/music.py): 136 BPM, output beat 0 at
// 0.015 s. Phrases start every 16 beats from beat 6; the first drop is beat 6, the splice is beat 70
// and the song's own ending starts at beat 102. The bass drops out on beats 34-37, 66-69 and 98-101.
export const FPS = 30;
export const BPM = 136;
export const BEAT_S = 60 / BPM;
export const OFFSET_S = 0.015;

export const bf = (beat: number) => Math.round((OFFSET_S + beat * BEAT_S) * FPS);

// Position in beats at a frame.
export const beatAt = (frame: number) => (frame / FPS - OFFSET_S) / BEAT_S;

// 1 on every beat, decaying before the next one.
export const pulse = (frame: number, decay = 6) => {
  const t = beatAt(frame);
  if (t < 0) return 0;
  return Math.exp(-(t - Math.floor(t)) * decay);
};
