#!/bin/bash
# Two-pass loudnorm to -14 LUFS (peaks at -1.5 dBTP) and web-safe H.264 (yuv420p, TV range, faststart).
# usage: prep/ig.sh in.mp4 out.mp4 [audio_lead_seconds_to_trim]
set -e
in=$1; out=$2; lead=${3:-0}
J=$(ffmpeg -hide_banner -nostats -i "$in" -af atrim=start=${lead},loudnorm=I=-14:TP=-1.5:LRA=11:print_format=json -f null - 2>&1 | sed -n '/^{/,/^}/p')
mi=$(echo "$J" | python3 -c "import json,sys;d=json.load(sys.stdin);print(f\"measured_I={d['input_i']}:measured_TP={d['input_tp']}:measured_LRA={d['input_lra']}:measured_thresh={d['input_thresh']}:offset={d['target_offset']}\")")
ffmpeg -v error -i "$in" -vf "scale=in_range=full:out_range=tv,format=yuv420p" -c:v libx264 -preset slow -crf 17 -maxrate 25M -bufsize 50M \
  -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
  -af "atrim=start=${lead},asetpts=PTS-STARTPTS,loudnorm=I=-14:TP=-1.5:LRA=11:${mi}:linear=true,aresample=48000" -c:a aac -b:a 320k -movflags +faststart "$out" -y
