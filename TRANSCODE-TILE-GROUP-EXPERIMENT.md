# MT8183 MM21 grouped-tile detiling — experiment

Experimental change to `libva-v4l2-mt8183`, based on the previous *padding-only* variant. No kernel/module/firmware changes.

## Changes

- `LIBVA_V4L2_TRANSCODE_TILE_GROUP=1` (default): original tile-by-tile traversal.
- `LIBVA_V4L2_TRANSCODE_TILE_GROUP=4`: process four adjacent 16-byte tiles as a group, row-by-row.
- `LIBVA_V4L2_TRANSCODE_TILE_GROUP=8`: process eight adjacent tiles as a group, row-by-row.
- A grouped traversal aims to improve **destination write locality** in NV12M DMA-BUF. It trades away some source sequentiality; performance on MT8183 is not yet known.
- Applies only when `LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1`. `LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1` is recommended from the previous experiment.
- Other software-decoder / VA-API conversion paths retain the default tile-by-tile behaviour.
- No manually inserted NEON or prefetch instructions: this isolates *memory traversal* first.

## Test

Build as before, then from the project directory run three passes at each setting (interleave 1/4/8 to reduce temperature/governor bias):

```bash
for group in 1 4 8; do
  LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 \
  LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 \
  LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1 \
  LIBVA_V4L2_TRANSCODE_TILE_GROUP="$group" \
  LIBVA_V4L2_ENC_DEBUG=1 \
  ffmpeg -hide_banner -loglevel warning -stats \
    -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 \
    -i /tmp/mt8183-high-input.mp4 \
    -c:v h264_vaapi -profile:v constrained_baseline \
    -b:v 5M -bf 0 -g 30 -frames:v 300 \
    -y "/tmp/mt8183-tile-${group}.mp4" \
    2>&1 | tee "/tmp/mt8183-tile-${group}.log"
done
```

Check decode, SHA256 and timings:

```bash
for group in 1 4 8; do
  ffmpeg -v error -xerror -i "/tmp/mt8183-tile-${group}.mp4" -f null - || exit 1
  printf 'group=%s ' "$group"
  ffmpeg -v error -i "/tmp/mt8183-tile-${group}.mp4" -map 0:v:0 -c:v copy -f h264 - | sha256sum
  grep -E 'TRANSCODE TILE GROUP|TRANSCODE DIRECT CONVERT timing|VENC input statistics' "/tmp/mt8183-tile-${group}.log"
done
```

All three extracted H.264 SHA256 hashes **must be identical**, along with 300 direct-convert frames and 0 fallbacks. Compare `detile_ms`, `total_ms` and final `speed`; the overall FFmpeg performance can be dominated by other work.

## Safety / limitations

- The ARM64 device test has **not** been run by the author of this patch.
- C++20 syntax check was performed for changed `src/convert.cc`, `src/surface.cc`, `src/h264_encode.cc`; standalone MM21 reference tests passed for group sizes 1, 4 and 8, including odd widths and padded strides.
- If results regress, keep group=1. Do not enable grouped mode by default until verified over longer runs.
