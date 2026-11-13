# MT8183: experimental MM21 -> encoder NV12M direct CPU conversion

This is an **opt-in experiment**. No kernel or MDP3 changes.

Baseline (default): VDEC MM21 -> linear NV12 in a decoder-owned CPU buffer -> CPU copy to two NV12M DRM DMA-BUF VENC staging planes.

With `LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1` and `LIBVA_V4L2_ENC_DMABUF=1`:
VDEC MM21 -> CPU detile straight into two VENC DMA-BUF staging planes -> VENC QBUF.

## Critical restrictions

* Only use this mode for **VAAPI decode -> VAAPI encode of the same frames** without a filter needing access to decoded image bytes. The opt-in deliberately defers the decoder's normal conversion, so vaDeriveImage, VA readback, export or other consumers of the intermediate decoder surface must NOT be used in this mode. Normal operation is unchanged when the flag is unset.
* It is **not** zero-copy: the CPU performs MM21 detiling, but avoids an extra full-frame NV12 copy.
* Requires real decoder MM21 CAPTURE and two DMA-BUF input planes on VENC; otherwise the driver materializes the standard linear NV12 conversion and falls back.
* Device not available in development environment: C++ syntax was checked, functional/device validation is required.
* The initial run should use 30 or 60 frames, then validate decoded result before comparing FPS with baseline.

## Test

Build normally, then:

```bash
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 \
LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel verbose \
  -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 \
  -i /tmp/mt8183-high-input.mp4 \
  -c:v h264_vaapi -profile:v constrained_baseline -b:v 5M -bf 0 -g 30 \
  -frames:v 60 -y /tmp/mt8183-direct-convert-60.mp4 \
  2>&1 | tee /tmp/mt8183-direct-convert-60.log

ffmpeg -v error -i /tmp/mt8183-direct-convert-60.mp4 -f null -
grep -E 'TRANSCODE DIRECT CONVERT|VENC input statistics|[Ee]rror|[Ff]ailed' \
  /tmp/mt8183-direct-convert-60.log
```

Expected on eligible input: `TRANSCODE DIRECT CONVERT stats: frames=60 fallbacks=0`; normal VENC counters will still record QBUFs as `dmabuf_staging` because the two VENC buffers are still allocated as staging planes, despite no staging copy.

For baseline comparison run exact same command **without** `LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1`. Compare wall-clock/processing speed and check output picture (not only successful decoding). Different scheduling or CPU load can affect FPS.
