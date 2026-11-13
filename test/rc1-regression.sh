#!/usr/bin/env bash
set -euo pipefail

: "${VAAPI_DEVICE:=/dev/dri/renderD128}"
: "${FRAMES:=300}"
export LIBVA_DRIVERS_PATH="${LIBVA_DRIVERS_PATH:-$PWD/build/src}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-v4l2}"

command -v ffmpeg >/dev/null || { echo "ffmpeg not found" >&2; exit 1; }

for dmabuf in 0 1; do
  echo "=== RC1 encode: ENC_DMABUF=$dmabuf, frames=$FRAMES ==="
  log=$(mktemp)
  if LIBVA_V4L2_ENC_DMABUF="$dmabuf" LIBVA_V4L2_ENC_DEBUG=1 \
    ffmpeg -hide_banner -loglevel warning -nostats \
      -vaapi_device "$VAAPI_DEVICE" \
      -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
      -vf 'format=nv12,hwupload' \
      -c:v h264_vaapi -profile:v high -b:v 8M -bf 0 -g 30 \
      -frames:v "$FRAMES" -f null - 2>"$log"; then
    grep 'VENC input statistics' "$log" || { cat "$log"; rm -f "$log"; exit 1; }
    if [[ $dmabuf = 0 ]]; then pattern="mmap=$FRAMES"; else pattern="dmabuf_staging=$FRAMES"; fi
    if ! grep -q "$pattern" "$log"; then cat "$log"; rm -f "$log"; exit 1; fi
    if grep -Eqi 'CPU sync.*failed|DMA-BUF.*failed' "$log"; then cat "$log"; rm -f "$log"; exit 1; fi
  else
    cat "$log"; rm -f "$log"; exit 1
  fi
  rm -f "$log"
done

if [[ ${1:-} == --transcode ]]; then
  input=${2:-}
  if [[ ! -f $input ]]; then echo "Usage: $0 --transcode /path/to/input.mp4" >&2; exit 2; fi
  echo "=== RC1 transcode: $FRAMES frames ==="
  log=$(mktemp)
  if LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 \
    LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 \
    LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1 \
    LIBVA_V4L2_TRANSCODE_TILE_GROUP=1 \
    LIBVA_V4L2_TRANSCODE_MEMORY_MODE=direct \
    LIBVA_V4L2_TRANSCODE_DETILE_ORDER=tile \
    LIBVA_V4L2_TRANSCODE_PREFETCH=0 LIBVA_V4L2_ENC_DEBUG=1 \
    ffmpeg -hide_banner -loglevel warning -nostats \
      -hwaccel vaapi -hwaccel_output_format vaapi \
      -vaapi_device "$VAAPI_DEVICE" -i "$input" \
      -c:v h264_vaapi -profile:v constrained_baseline \
      -b:v 5M -bf 0 -g 30 -frames:v "$FRAMES" -f null - 2>"$log"; then
    grep -E 'VENC input statistics|TRANSCODE DIRECT CONVERT stats' "$log" || true
    if ! grep -q "TRANSCODE DIRECT CONVERT stats: frames=$FRAMES fallbacks=0" "$log"; then
      cat "$log"; rm -f "$log"; exit 1
    fi
  else
    cat "$log"; rm -f "$log"; exit 1
  fi
  rm -f "$log"
fi

echo 'RC1 regression PASS (tested paths only)'
