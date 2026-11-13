# MT8183 experimental direct VA-surface → V4L2 DMA-BUF input

This is an **opt-in experiment**, not yet validated on MT8183 hardware.

* `LIBVA_V4L2_ENC_DMABUF=1` enables the established DRM staging DMA-BUF V4L2 input queue.
* `LIBVA_V4L2_ENC_DIRECT=1` additionally asks VA `vaPutImage()` to allocate each encoder input surface as **two separate DRM PRIME buffers** (Y and interleaved UV). At `VIDIOC_QBUF`, the encoder attempts to import these *source* buffers directly, avoiding the staging copy.
* Both settings must be `1` to enable the direct path. With `DIRECT` unset, behavior is unchanged.
* `LIBVA_V4L2_ENC_DEBUG=1` prints diagnostics. Look for **`VENC direct PRIME QBUF (no encoder staging copy)`**. A successful MP4 alone does not prove direct import.
* If direct allocation fails, existing staging is used. If direct QBUF fails before STREAMON, the encoder reverts to the established MMAP path.
* `vaPutImage()` still copies **system-memory NV12 into the surface**, so this is not end-to-end zero-copy. The optimization removes only the *additional* copy from the VA surface to the V4L2 OUTPUT buffer. For real GPU/decoder → encoder zero-copy we would need further negotiation of the producer's DRM modifiers and plane layout.
* Some applications use `vaDeriveImage()` rather than `vaPutImage()` to upload data. This first experiment optimizes **only the vaPutImage path**, and other upload paths use the original staging implementation.
* The surface PRIME buffers must remain alive until V4L2 DQBUF. The current encoder waits synchronously for completion before releasing the frame; do not repurpose these surfaces for asynchronous encoding without adding explicit ownership/fences.

## Test

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" \
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 \
LIBVA_V4L2_ENC_DIRECT=1 \
LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -loglevel verbose \
  -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
  -vf 'format=nv12,hwupload' \
  -c:v h264_vaapi -profile:v constrained_baseline \
  -b:v 10M -bf 0 -g 30 -frames:v 300 \
  -y /tmp/mt8183-vaapi-prime-direct-1080p.mp4
```

Check `ffprobe` resolution and frame count, visually review the MP4, and compare speed against `LIBVA_V4L2_ENC_DIRECT=0` under the same settings. The first green frame seen in earlier tests may persist; it is not addressed here.

## Known limits

No kernel changes required in principle, but DRM PRIME import by `vb2_dma_contig` still needs real testing with the selected DRM allocator. The computed allocation follows MT8183's observed 16-pixel width / 32-line height alignment plus extra queue plane padding. This experimental path does not support imported/tiled surfaces directly; they retain the copying path. DMA-BUF synchronization errors and invalid plane geometry must be investigated on the real device.
