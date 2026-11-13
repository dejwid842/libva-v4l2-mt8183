# MT8183: decoder CAPTURE -> encoder OUTPUT compatibility probe

This version is based on `libva-v4l2-mt8183-prime-scoped-stable-hwupload.zip`.
It does **not** implement decoder-to-encoder zero-copy yet. It examines the buffer
from the first decoded source passed to the encoder in read-only diagnostic mode.

## Run

Use the previously generated `/tmp/mt8183-high-input.mp4` (H.264 High, 1920x1080).
Do not enable `LIBVA_V4L2_ENC_FORCE_PUTIMAGE`.

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" \
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 \
LIBVA_V4L2_ENC_DIRECT=1 \
LIBVA_V4L2_TRANSCODE_PROBE=1 \
LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -hide_banner -loglevel verbose \
  -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 \
  -i /tmp/mt8183-high-input.mp4 \
  -c:v h264_vaapi -profile:v constrained_baseline \
  -b:v 5M -bf 0 -g 30 -frames:v 300 \
  -y /tmp/mt8183-hw-transcode-probe.mp4 \
  2>&1 | tee /tmp/mt8183-hw-transcode-probe.log

grep -E 'TRANSCODE PROBE|VENC input statistics|Conversion failed' \
  /tmp/mt8183-hw-transcode-probe.log
```

## Interpretation

* `VDEC CAPTURE fourcc=MM21` or `conversion=1` means tiled MT8183 output.
  Importing this as linear V4L2 NV12M would produce corruption; a hardware
  format-conversion path or other compatible allocations are required.
* `VDEC EXPBUF success` means only that the decoder CAPTURE buffer can be
  exported. It does **not** imply the pixel layout is usable by the encoder.
* `decoder capture compatible with encoder NV12M=1` indicates a potential
  direct-import candidate, **not** proof: lifetime, cache synchronization,
  queue ownership, and re-use must be addressed separately.
* `converted_exportable=0` indicates a converted CPU buffer cannot itself
  be passed as DMA-BUF on this kernel.
* The existing encoder staging and Direct PRIME upload modes remain unchanged.

This probe only performs a temporary `VIDIOC_EXPBUF` and closes the returned
FDs. No exported decoder FD is queued to the encoder.
