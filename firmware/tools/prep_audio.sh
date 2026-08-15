#!/usr/bin/env bash
# Convert the web app's mp3 cues into raw PCM the ES8311 can be fed directly.
#
# The codec runs at a fixed 16 kHz mono (ZECTRIX_AUDIO_SAMPLE_RATE), so there is
# no decoder on the device: the clips are embedded as headerless s16le and
# written straight to I2S.
#
# The onboard speaker is small, so each clip is peak-normalised and then pushed
# a few dB into a limiter. That trades a little dynamic range for the loudness
# that actually matters in a room.
set -euo pipefail

SRC_DIR="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
OUT_DIR="${2:-$(cd "$(dirname "$0")/.." && pwd)/main/assets}"

RATE=16000
PUSH_DB=4  # extra gain driven into the limiter

mkdir -p "$OUT_DIR"

for name in chime alert; do
    src="$SRC_DIR/$name.mp3"
    out="$OUT_DIR/${name}_16k.pcm"
    [ -f "$src" ] || { echo "missing $src" >&2; exit 1; }

    peak=$(ffmpeg -hide_banner -nostats -i "$src" -af volumedetect -f null - 2>&1 |
           sed -n 's/.*max_volume: \(-*[0-9.]*\) dB.*/\1/p')
    gain=$(echo "$peak $PUSH_DB" | awk '{printf "%.1f", -$1 + $2}')

    ffmpeg -hide_banner -loglevel error -y -i "$src" \
        -af "volume=${gain}dB,alimiter=limit=0.98:attack=2:release=40" \
        -ac 1 -ar "$RATE" -f s16le "$out"

    bytes=$(wc -c < "$out")
    printf '%-12s peak %6s dB  gain %+6s dB  %7d bytes  %.2fs\n' \
        "$name" "$peak" "$gain" "$bytes" "$(echo "$bytes $RATE" | awk '{print $1/2/$2}')"
done
