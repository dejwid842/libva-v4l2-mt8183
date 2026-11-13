# MT8183 – padding-only writes during MM21 -> NV12M transcode

Experimental userspace-only change. No kernel rebuild. Based on `libva-v4l2-mt8183-transcode-direct-convert-experiment.zip`.

## Select mode

Enable direct transcode conversion: `LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1`.

* Existing full-plane initialization (control): `LIBVA_V4L2_TRANSCODE_PADDING_ONLY=0` (default).
* Optimize initialization outside the active rectangle: `LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1`.

`LIBVA_V4L2_ENC_DEBUG=1` enables end-of-run counters and timings. The timer for padding covers the start of DMA-BUF CPU access and initialization; the timer for detile covers only the tiling conversion; the total also includes end-of-access synchronization. Timings are accumulated wall-clock milliseconds on the encoder submission path (not standalone hardware decode or encode timings).

The new path initializes right-hand stride padding, bottom coded rows, and trailing mapped slack with the same neutral NV12 values as before, without writing image bytes that the MM21 detiler will immediately overwrite.

## First test: 60 frames

```bash
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 \
LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 \
LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1 LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel warning -stats \
  -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i /tmp/mt8183-high-input.mp4 \
  -c:v h264_vaapi -profile:v constrained_baseline -b:v 5M -bf 0 -g 30 \
  -frames:v 60 -y /tmp/mt8183-padding-60.mp4 \
  2>&1 | tee /tmp/mt8183-padding-60.log
ffmpeg -v error -xerror -i /tmp/mt8183-padding-60.mp4 -f null -
```

Expect `TRANSCODE DIRECT CONVERT stats: frames=60 fallbacks=0` and `TRANSCODE DIRECT CONVERT timing: ... padding_only=60`.

## A/B comparison (300 frames)

Run the same FFmpeg command using `-frames:v 300`, first with `LIBVA_V4L2_TRANSCODE_PADDING_ONLY=0`, then `=1`. Keep other settings identical; alternate A/B/A/B to reduce effects from device clock/thermal changes. Compare final FFmpeg fps/speed, timing sums, and output SHA-256 of the H.264 elementary streams. **Identical SHA-256 is required before accepting the optimized path**; decoding success alone is insufficient.

Do not enable `LIBVA_V4L2_ENC_FORCE_PUTIMAGE=1` for mixed VAAPI decode+encode. Do not use Direct Convert with filters or downstream consumers that read the unmaterialized decoder NV12 surface. This test does not provide GPU zero-copy.

No MT8183 hardware was available in the build environment; syntax and archive verification do not prove functionality or performance.
