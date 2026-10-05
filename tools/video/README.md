# Promo video (Remotion)

```sh
npm install
# screen clip: the README demo's screen, cleaned back to device pixels and scaled 6x (not committed)
ffmpeg -i ../../docs/media/demo.mp4 -vf "crop=720:405:280:150,scale=240:135:flags=neighbor,scale=1440:810:flags=neighbor" \
  -c:v libx264 -crf 12 -preset slow -pix_fmt yuv420p -g 15 -an public/clips/screen.mp4
# soundtrack: Pixabay 243987 "Intense Electro Trailer Music" (JkStudios) as prep/electro-trailer.wav, then
python prep/music.py                # one splice -> public/music.wav (needs numpy + soundfile)
npm run studio                      # preview
npm run render                      # -> out/pocket-notes-promo-master.mp4 
bash prep/finish.sh out/pocket-notes-promo-master.mp4 out/pocket-notes-promo.mp4 0.042
# README copy: GitHub's inline player takes uploads up to 10 MB, so 2-pass to ~9.5 MB
ffmpeg -i out/pocket-notes-promo.mp4 -c:v libx264 -preset slower -b:v 1450k -pass 1 -an -f null /dev/null
ffmpeg -i out/pocket-notes-promo.mp4 -c:v libx264 -preset slower -b:v 1450k -maxrate 4M -bufsize 6M -pass 2 \
  -pix_fmt yuv420p -c:a aac -b:a 128k -movflags +faststart out/pocket-notes-promo-web.mp4
```

- Scene timing is in song beats (`src/beats.ts`, 136 BPM); every cut lands on a beat. The video opens 6 beats before
  the first drop, so the device's 88% pick lands on it at 2.6 s.
- All on-screen text is in `src/copy.ts`.
- The device footage is real time and uncut inside each shot: the 88% pick lands on beat 6 and the 68% pick
  on beat 66 (`SHOTS` in `src/Promo.tsx`).
- `prep/finish.sh` trims Remotion's 42 ms AAC lead and normalises to -14 LUFS / -1.5 dBTP.
