# V4L2 libVA Backend
This libVA backend is designed to work with the [Video for Linux Memory-To-Memory API](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/dev-mem2mem.html) that is used by a number of video codecs drivers, in particular SoCs found on SBCs.
After an initial implementation by Bootlin, development on this backend ceased before the Stateless V4L2-M2M API it uses became stable.
Development has since been picked up again, see [#Status] for details.

## Building
The project is built using meson:
```
meson setup build
meson compile -Cbuild
```

## Usage
Applications using the backend can be launched by adding the build directory to the libVA driver path, and, optionally, setting the driver name to load:
```
LIBVA_DRIVERS_PATH=<project dir>/build/src LIBVA_DRIVER_NAME=v4l2 vainfo
```

Alternatively, the library can be installed to the default driver path:
```
meson install -Cbuild
```

The driver probes the system for appropriate V4L2 devices, advertising all of their capabilities.
This can be overriden by explicitly specifying a device pair to use:
```
export LIBVA_V4L2_VIDEO_PATH=/dev/videoX LIBVA_V4L2_MEDIA_PATH=/dev/mediaY
```

Note that some applications need further configuration to load the library.
In particular, gstreamer based applications have a whitelist for supported drivers, that can be disabled manually (`GST_VAAPI_ALL_DRIVERS=1`).

## MediaTek MT8183 (and other MM21 decoders)
The `mtk-vcodec` stateless decoder only produces `MM21`, a 16x32 tiled variant of NV12. The backend detiles each picture
on the CPU into a linear NV12 DMA-BUF when the surface is synchronised, so applications see a normal NV12 surface
(`vaDeriveImage`, `vaGetImage` and DRM PRIME export all work).
The buffer is allocated from `/dev/dma_heap/system` (override with `LIBVA_V4L2_DMA_HEAP=<heap name or path>`),
falling back to `/dev/udmabuf`. Make sure your user may access one of them.
Without either (kernel lacking `CONFIG_DMABUF_HEAPS_SYSTEM`/`CONFIG_UDMABUF`) plain memory is used: `mpv --hwdec=vaapi-copy`
and other users of `vaGetImage`/`vaDeriveImage` work, but zero-copy export (`--hwdec=vaapi`) does not.
`LIBVA_V4L2_BUFFERS=<n>` sets how many V4L2 buffers are allocated at first (default 24; more are added when needed,
if the driver lets us).
Applications that create the context without listing surfaces (ffmpeg's dynamic pool) are supported: buffers are
then allocated (24 of them) when a surface is first decoded into.
Verify the conversion without hardware using `g++ -std=c++20 -Isrc test/mm21_test.cc src/convert.cc src/dmabuf.cc`.

Exporting pictures (`vaExportSurfaceHandle`, as used by Firefox or `mpv --hwdec=vaapi`) needs DMA-BUFs. If the kernel has
neither dma-heap nor udmabuf, the converted picture is copied into a buffer of the display driver (a DRM dumb buffer from
`/dev/dri/card*`), which can be exported, and needs nothing from the kernel but that driver
(`LIBVA_V4L2_QUIRKS=nodrm` switches it off). If that isn't possible either, the MediaTek MDP3 converter (`/dev/videoN` of the `mtk-mdp3` driver) is used instead, which
produces exportable NV12 buffers from the decoder's pictures without the CPU. `LIBVA_V4L2_QUIRKS=nomdp3` switches it
off, `LIBVA_V4L2_MDP3_PATH=/dev/videoN` selects the device. (MDP3 only works if its device tree node has the
`mediatek,scp` property, which some trees lack.) `vaDeriveImage` and `vaGetImage` don't need it.

Applications can also hand the driver the memory for a surface (`vaCreateSurfaces` with `VASurfaceAttribExternalBuffers` or
a `VADRMPRIMESurfaceDescriptor`, as Chromium does): the decoded pictures are then converted straight into that buffer.
It has to be one linear NV12 buffer. Surfaces that are exported before anything is decoded into them (Chromium's other
mode) get a buffer that is kept and updated with every picture decoded into the surface.

Diagnostics: `LIBVA_V4L2_DUMP=/some/dir` writes the raw planes of the first three decoded pictures (`frameN.plane0.raw`,
`frameN.plane1.raw`, converted `frameN.nv12` and the driver's format in `frameN.info.txt`), and prints the capture
format at setup. It also writes what is sent to the decoder: `picN.source.bin` (the bitstream) and `picN.controls.txt`.
If decoding a picture fails (e.g. the request times out), `failN.source.bin`, `failN.controls.txt` and
`failN.va_picture.bin` describe that picture.
`LIBVA_V4L2_TRACE` (set to anything) also makes the driver say how surfaces are created: with its own buffers, or
the application's.
`LIBVA_V4L2_TRACE=<file>` appends, for every picture, the controls sent to the decoder and the reference lists the
application used. `LIBVA_V4L2_QUIRKS=nocache,rawpoc` switches off individual assumptions (cacheable capture buffers,
order counts relative to the IDR picture), to find out which of them matters.
`LIBVA_V4L2_PROFILE=1` prints average and maximum time spent in the hardware decoder, in the MM21 conversion and in
copies to `VAImage`s every 60 pictures, to find out where time goes when playback is not smooth.

## Status
The decoding path currently supports these codecs: MPEG2, H264, VP8, (and VP9). The initial MT8183 H.264 encoding path
is experimental and described above.
VP9 support depends on a part of gstreamer that is not likely to be present in the version shipped by your distribution.
The implementation has been tested using Intel's [vaapi-fits](https://github.com/intel/vaapi-fits) on an RK3399, which is supported by the `hantro` and `rockchip` drivers.
Feedback on results for other platforms are very welcome, do not expect the library to simply work smoothly though.
Future development on this project aims to improve stability, add supported codecs, and support the stateful V4L2-M2M API.

## MediaTek MT8183 H.264 encoding (initial implementation)

An initial `VAEntrypointEncSlice` path is included for stateful V4L2 encoders that expose `NV12M` on
`VIDEO_OUTPUT_MPLANE` and `H264` on `VIDEO_CAPTURE_MPLANE` (including the MT8183 `mtk-vcodec-enc` node).
The V4L2 node is discovered from the media topology; its `/dev/videoN` number is not fixed.

The first implementation is intentionally conservative: it uses four MMAP buffers per queue and copies a linear NV12
surface into the V4L2 encoder's two-plane input buffers on the CPU. Full-frame, same-size NV12 uploads through
`vaPutImage()` and mapped `vaDeriveImage()` surfaces are supported; crop and scaling in `vaPutImage()` are not.
H.264 SPS/PPS are requested in the first frame,
rate control is CBR, B-frames are disabled, and the V4L2 controls are configured from the VA sequence and rate-control
parameters where possible. The VA frame-rate hint currently cannot be applied because the tested MT8183 driver rejects
`VIDIOC_S_PARM`/`VIDIOC_G_PARM`. Packed headers, ROI/QP maps, B-frames and direct DMA-BUF queueing are not implemented.

The MT8183 encoder aligns raw input height to 32 lines. On the tested kernel, a requested 1280x720 input is therefore
reported back as 1280x736 and the resulting H.264 elementary stream is also reported as 1280x736. The tested driver
rejects the selection ioctls needed to set a 720-line visible crop; this is a kernel-driver limitation, not a libva
crop implementation. Verify the bitstream dimensions on the target kernel before relying on display aspect/cropping.

A first hardware smoke test can be run with FFmpeg (replace the render node if needed):

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
ffmpeg -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i 'testsrc2=size=1280x720:rate=30' \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -profile:v high -b:v 4M -g 60 \
  -frames:v 120 -y /tmp/mt8183-vaapi-encode.mp4

ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,profile,width,height,level \
  -of default=noprint_wrappers=1 /tmp/mt8183-vaapi-encode.mp4
ffmpeg -v error -i /tmp/mt8183-vaapi-encode.mp4 -f null -
```

This path is an initial implementation and still needs compilation against the target system's libva development
headers and on-device validation. In particular, verify SPS/PPS placement, output dimensions, bitrate/GOP controls,
error recovery, and the actual frame rate before treating it as production-ready.
