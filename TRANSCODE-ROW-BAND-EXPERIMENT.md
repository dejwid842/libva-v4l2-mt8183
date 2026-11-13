# MT8183 MM21 — row-major band traversal experiment

This is a user-space experiment; no kernel, firmware, or module rebuild is required.
It is based on the most recent `libva-v4l2-mt8183-memory-benchmark` source.

## What changes

`LIBVA_V4L2_TRANSCODE_DETILE_ORDER=tile` (default): existing detiling loop, tile-by-tile (with optional tile grouping). With `TILE_GROUP=1` this preserves the measured reference path.

`LIBVA_V4L2_TRANSCODE_DETILE_ORDER=row`: within each 32-row luma or 16-row chroma tile band, traverse output rows in ascending destination address order. For each destination row copy 16-byte spans from consecutive MM21 tiles. Cropped or incomplete tile boundaries still copy only visible bytes. `TILE_GROUP` is ignored by `row` mode.

All other behaviors, including the explicit `PADDING_ONLY` mode, `MEMORY_MODE=direct` and fallback, are preserved. Both traversal variants produce identical bytes in a host-side reference test for 195 image dimensions (including widths/heights not divisible by tile size), including the padding left unchanged by the converter. These tests do **not** prove device-level output integrity; compare actual encoded H264 hashes.

## Device test

Build the driver as usual, then from the project root:

```sh
for order in tile row row tile; do
  stamp=$(date +%s%N)
  LIBVA_DRIVERS_PATH="$PWD/build/src" \
  LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_V4L2_ENC_DMABUF=1 \
  LIBVA_V4L2_ENC_DIRECT=1 \
  LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 \
  LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1 \
  LIBVA_V4L2_TRANSCODE_TILE_GROUP=1 \
  LIBVA_V4L2_TRANSCODE_MEMORY_MODE=direct \
  LIBVA_V4L2_TRANSCODE_DETILE_ORDER="$order" \
  LIBVA_V4L2_ENC_DEBUG=1 \
  ffmpeg -hide_banner -loglevel warning -stats \
    -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 \
    -i /tmp/mt8183-high-input.mp4 \
    -c:v h264_vaapi -profile:v constrained_baseline \
    -b:v 5M -bf 0 -g 30 -frames:v 300 \
    -y "/tmp/mt8183-row-${order}-${stamp}.mp4" \
    2>&1 | tee "/tmp/mt8183-row-${order}-${stamp}.log"
done
```

Check the SHA-256 **of the H.264 elementary streams** (not the MP4 containers):

```sh
for file in /tmp/mt8183-row-*.mp4; do
  echo "=== $file ==="
  ffmpeg -v error -xerror -i "$file" -f null - || echo "DECODE FAILED"
  ffmpeg -v error -i "$file" -map 0:v:0 -c:v copy -f h264 - | sha256sum
done
grep -H -E 'TRANSCODE DETILE ORDER|TRANSCODE MEMORY BENCH|TRANSCODE DIRECT CONVERT timing|frame= *300' /tmp/mt8183-row-*.log
```

Compare `dma_detile_cpu_ms` as well as `total_ms` and final FFmpeg speed. The output bitstreams should match the earlier baseline (`035be45a4cd15ed65e569fecfbc1fc88a6856428a52433f935b48bb0b9a1e49d`) given unchanged video input, encoder parameters and driver behavior. Timing variability remains substantial on the tested MT8183; several alternating runs are required.

If `row` is slower, use `LIBVA_V4L2_TRANSCODE_DETILE_ORDER=tile` or leave the variable unset. No NEON, prefetch, DMA-BUF synchronization, decoder, or VENC queue behavior has been modified.
