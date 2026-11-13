# MT8183: MM21 source prefetch experiment

This is a user-space only experiment. No kernel headers/modules/firmware changes.
Based on **Row Band Experiment**. The reference settings remain `DETILE_ORDER=tile` and `TILE_GROUP=1`, `PADDING_ONLY=1`, `MEMORY_MODE=direct`.

## Change

`LIBVA_V4L2_TRANSCODE_PREFETCH=0` (default): no additional prefetch.
`LIBVA_V4L2_TRANSCODE_PREFETCH=1`: hint to load the next MM21 tile before processing the current tile.
`LIBVA_V4L2_TRANSCODE_PREFETCH=2`: hint to load the tile two positions ahead.

The new `__builtin_prefetch` hints target **source** MM21 data only, with explicit range checks. They do not touch the output DMA-BUF and are not expected to alter the H264 result. They are advisory hardware hints; performance can improve, stay unchanged or regress. The reference test exercises tile groups 1/4/8 and prefetch 0/1/2 on small, odd and HD image sizes.

## Device experiment

Build the driver normally, then run 60 frames with each setting (check H264 SHA256 and decode), followed by repetitions with 300 frames. Example:

```sh
for distance in 0 1 2; do
  LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 \
  LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 \
  LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1 \
  LIBVA_V4L2_TRANSCODE_TILE_GROUP=1 \
  LIBVA_V4L2_TRANSCODE_MEMORY_MODE=direct \
  LIBVA_V4L2_TRANSCODE_DETILE_ORDER=tile \
  LIBVA_V4L2_TRANSCODE_PREFETCH="$distance" \
  LIBVA_V4L2_ENC_DEBUG=1 \
  ffmpeg -hide_banner -loglevel warning -stats \
    -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 \
    -i /tmp/mt8183-high-input.mp4 \
    -c:v h264_vaapi -profile:v constrained_baseline \
    -b:v 5M -bf 0 -g 30 -frames:v 300 \
    -y "/tmp/mt8183-prefetch-${distance}.mp4" \
    2>&1 | tee "/tmp/mt8183-prefetch-${distance}.log"
done

for distance in 0 1 2; do
  ffmpeg -v error -xerror -i "/tmp/mt8183-prefetch-${distance}.mp4" -f null - || break
  ffmpeg -v error -i "/tmp/mt8183-prefetch-${distance}.mp4" \
    -map 0:v:0 -c:v copy -f h264 - | sha256sum
  grep -E 'TRANSCODE PREFETCH|TRANSCODE MEMORY BENCH|TRANSCODE DIRECT CONVERT timing|frame= *300' \
    "/tmp/mt8183-prefetch-${distance}.log"
done
```

For meaningful comparative results, run an interleaved sequence such as `0 1 2 2 1 0`, retaining run-specific logs. Compare `dma_detile_cpu_ms`, `direct_wall_ms` and FFmpeg speed, not just best-case runs. A prefetch hint is not a CPU/cache fix by itself.
