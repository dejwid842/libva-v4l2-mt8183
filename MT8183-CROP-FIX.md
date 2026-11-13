# MT8183 visible-resolution crop fix

Kernel patch required: `mt8183-kernel-venc-selection-mplane.patch` changes the MediaTek VENC `VIDIOC_{G,S}_SELECTION` type checks to `VIDEO_OUTPUT_MPLANE`. The backend attempts `VIDIOC_S_SELECTION` after S_FMT and before streaming, setting the visible crop to the VA context size (e.g. 1280x720) while keeping the padded NV12M plane sizes (1280x736).

Apply the kernel patch from the Linux source root and rebuild/install the affected kernel/module. Until the patched kernel is running, the backend logs a warning and keeps the old encoded dimensions.

Verify with `ffprobe -show_entries stream=profile,width,height,level` and full-stream decode. Both changes are experimental and need a real MT8183 test.
