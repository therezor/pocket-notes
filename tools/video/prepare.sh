#!/bin/sh
# Cut the demo clips (3x speed, 6x pixel scale) out of the simulator recording
# and copy in the soundtrack. Usage: tools/video/prepare.sh path/to/music.mp3
set -e
cd "$(dirname "$0")"
RAW=../sim/build/rec/demo.rgb
[ -f "$RAW" ] || ../sim/record.sh
mkdir -p public/clips
[ -n "$1" ] && cp "$1" public/music.mp3
# name  source-start  source-length (seconds of real device time; lengths are
# whole music bars x 3, so each clip ends on a bar line at 3x speed)
BAR=1.85759
while read -r name start bars; do
  len=$(echo "$bars * $BAR * 3" | bc -l)
  ffmpeg -nostdin -loglevel error -y -ss "$start" -t "$len" \
    -f rawvideo -pix_fmt rgb24 -s 240x135 -r 20 -i "$RAW" \
    -vf "setpts=PTS/3,fps=30,scale=1440:810:flags=neighbor" \
    -c:v libx264 -crf 14 -preset slow -pix_fmt yuv420p -an "public/clips/$name.mp4"
  echo "$name: $start s + $len s -> $(echo "$len / 3" | bc -l | cut -c1-5) s"
done <<CLIPS
talk     0.3  7
feelings 40.4 4
facts    62.7 6
story    96.4 5
CLIPS
