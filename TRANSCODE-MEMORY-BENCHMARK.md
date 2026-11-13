# MT8183 — MM21 detiling: RAM versus DMA-BUF benchmark

This is an experimental user-space driver build. It requires **no kernel or firmware changes**.

## Modes

Enable `LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1`, `LIBVA_V4L2_ENC_DMABUF=1`, `LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1`, and leave `LIBVA_V4L2_TRANSCODE_TILE_GROUP=1`.

`LIBVA_V4L2_TRANSCODE_MEMORY_MODE=direct` (default): detile MM21 directly into two VENC DMA-BUF mapped planes. Existing path, now with thread CPU timings.

`LIBVA_V4L2_TRANSCODE_MEMORY_MODE=ramcopy`: detile MM21 into two ordinary, reusable RAM buffers with matching plane sizes/strides, then one linear memcpy per plane into VENC DMA-BUF (while DMA_BUF_IOCTL_SYNC WRITE is active). This exposes both **A (RAM detile)** and **C (RAM detile + DMA-BUF copy)** while `direct` represents **B**.

Each mode produces the **same output bitstream** if the pipeline is correct. Check SHA-256 of elementary H.264 before interpreting timing comparisons.

## Test (300 frames)

Build as usual from the source tree. In the build directory run:

```sh
for mode in direct ramcopy ramcopy direct direct ramcopy; do
  stamp=$(date +%s%N)
  LIBVA_DRIVERS_PATH="$PWD/build/src" \
  LIBVA_DRIVER_NAME=v4l2 \
  LIBVA_V4L2_ENC_DMABUF=1 \
  LIBVA_V4L2_ENC_DIRECT=1 \
  LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 \
  LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1 \
  LIBVA_V4L2_TRANSCODE_TILE_GROUP=1 \
  LIBVA_V4L2_TRANSCODE_MEMORY_MODE="$mode" \
  LIBVA_V4L2_ENC_DEBUG=1 \
  ffmpeg -hide_banner -loglevel warning -stats \
    -hwaccel vaapi -hwaccel_output_format vaapi \
    -vaapi_device /dev/dri/renderD128 \
    -i /tmp/mt8183-high-input.mp4 \
    -c:v h264_vaapi -profile:v constrained_baseline \
    -b:v 5M -bf 0 -g 30 -frames:v 300 \
    -y "/tmp/mt8183-memory-${mode}-${stamp}.mp4" \
    2>&1 | tee "/tmp/mt8183-memory-${mode}-${stamp}.log"
done
```

For a quick first correctness check, run one direct and one ramcopy, then compare raw H.264 hashes:

```sh
for f in /tmp/mt8183-memory-*.mp4; do
    printf '%s ' "$f"
    ffmpeg -v error -i "$f" -map 0:v:0 -c:v copy -f h264 - | sha256sum
    ffmpeg -v error -xerror -i "$f" -f null - || echo 'DECODE FAILED'
done

grep -H -E 'TRANSCODE MEMORY BENCH|TRANSCODE DIRECT CONVERT timing|VENC input statistics|frame= *300' /tmp/mt8183-memory-*.log
```

## Timers

`TRANSCODE MEMORY BENCH` reports aggregate times across encoded frames:

- `direct_wall_ms`, `direct_cpu_ms`: whole direct branch including padding, synchronization and detile.
- `dma_detile_cpu_ms`: CPU time spent specifically in detile to DMA-BUF; compare to legacy `detile_ms` wall clock.
- `ram_detile_wall_ms`, `ram_detile_cpu_ms`: detile to ordinary RAM (A).
- `dma_copy_wall_ms`, `dma_copy_cpu_ms`: RAM → DMA-BUF linear plane copies **including DMA-BUF synchronization**.
- `ramcopy_wall_ms`, `ramcopy_cpu_ms`: entire RAM+copy branch including RAM buffer resize (after first use), padding, detile, copy and sync (C).

The memory modes are tested in separate FFmpeg runs; there is no double conversion within an individual frame. `direct` is the default even when this build is used without the benchmark env var. `ramcopy` is experimental. Existing `TRANSCODE DIRECT CONVERT timing` is retained, but its `total_ms` now includes RAM detile + copy in `ramcopy` mode.

Do not compare wall times alone when other applications are active; CPU times help isolate descheduling. CPU time may still change due to cache misses and memory stalls. Do not expect `ramcopy` to be faster; a copy can dominate.
