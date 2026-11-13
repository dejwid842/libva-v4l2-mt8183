# Experimental MT8183 H.264 V4L2 DMA-BUF input

This is an **opt-in V4L2 DMA-BUF import experiment**, **not zero-copy encoding**.

## Modes

- Default: V4L2 MMAP input (the previously tested, working encoder path).
- `LIBVA_V4L2_ENC_DMABUF=1`: allocate **two separate DRM dumb/PRIME buffers** for each V4L2 OUTPUT_MPLANE slot, one for Y and one for UV. Import both through `V4L2_MEMORY_DMABUF`. CPU still copies the input VA surface to these two buffers, honoring negotiated pitch and visible crop. DMA_BUF_IOCTL_SYNC brackets the CPU write.
- If DMA-BUF allocation / `REQBUFS` fails, the constructor falls back to MMAP. If the initial `VIDIOC_QBUF` rejects imported buffers before STREAMON, the driver releases the DMA-BUF OUTPUT queue and retries that frame with MMAP. Errors after streaming starts are **not** automatically retried because the device queue may already contain frames.

The decoder implementation and its DMA-BUF/DRM path are untouched. No kernel DMA-BUF heaps or UDMABUF configuration changes are needed; the experimental allocator relies on `/dev/dri/card*` with DRM dumb-buffer allocation and PRIME export support.

## Test

Start by testing 30 frames, then 300 frames. Preserve your prior MMAP test as a comparison.

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" \
LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 \
LIBVA_V4L2_ENC_DEBUG=1 \
ffmpeg -loglevel verbose -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi \
  -profile:v constrained_baseline -b:v 10M -bf 0 -g 30 \
  -frames:v 30 -y /tmp/mt8183-vaapi-dmabuf-1080p.mp4

ffprobe -v error -show_entries stream=codec_name,profile,width,height,level \
  -of default=noprint_wrappers=1 /tmp/mt8183-vaapi-dmabuf-1080p.mp4
ffmpeg -v error -i /tmp/mt8183-vaapi-dmabuf-1080p.mp4 -f null -
```

Look for `VENC DMA-BUF input enabled` and **absence** of `falling back to MMAP` or `QBUF rejected`. Successful MP4 alone does not prove DMA-BUF was used: a fallback may have occurred. This is an import-compatibility probe, not a performance optimization yet.

## True zero-copy (future)

The current encoder's source VA surface is typically a **single** NV12 allocation (Y and UV are offsets within that allocation). V4L2 NV12M imports two separately based planes; simply supplying the same fd twice with `data_offset` does not necessarily give the correct hardware addresses. True zero-copy needs a compatible multi-plane producer / allocator and an explicit lifetime + synchronization contract. Do not enable untested same-fd sharing.

## Korekta regresji FPS (2026-11-07)
Przywrócono ustawienie `VIDIOC_S_PARM` dla `V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE` oraz obsługę `VAEncMiscParameterFrameRate`. Poprzednia wersja archiwum eksperymentalnego nie zawierała tej poprawki, przez co bitrate mógł nie odpowiadać nastawie. W trybie debug oczekiwany komunikat: `encoder S_PARM requested FPS=30/1, returned timeperframe=1/30`.
