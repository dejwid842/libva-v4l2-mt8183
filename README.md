# libva-v4l2-mt8183 — v0.1.0-rc1

Release candidate for the **tested MT8183 environment**, not a general VA-API compatibility guarantee.
This release candidate is based on Audit 03 and includes the Audit 01–03 fixes.
The historical upstream README is preserved as [`README-UPSTREAM.md`](README-UPSTREAM.md).

## Known working environment

- MT8183 (`willow`), kernel `6.18.34-stb-cbm+`; FFmpeg 7.1.5; DRM render node `/dev/dri/renderD128`.
- H.264 High hardware decode through VA-API, H.264 hardware encode through V4L2, and 1080p30 VA-API transcode.
- VDEC outputs tiled MM21; userspace CPU detiling converts to linear NV12/NV12M. MT8183 MDP3 is **not** functional in the tested configuration (firmware `MDP_IPI_EINVAL`).
- VENC input modes: MMAP and DMA-BUF staging; direct PRIME is an optional separate path and is not the same as DMA-BUF staging.

## Build

```sh
meson setup build --buildtype=release -Db_lto=true
ninja -C build
```

For an already configured build tree, use `meson setup build --reconfigure --buildtype=release -Db_lto=true`.
Build requires the existing project's development dependencies, including libva, libdrm and libudev.
The Meson project version is fixed to `0.1.0-rc1` so a ZIP extraction does not require a Git repository.

## Basic encoding example (H.264 High)

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 \
ffmpeg -hide_banner -loglevel warning -nostats \
  -vaapi_device /dev/dri/renderD128 \
  -f lavfi -i 'testsrc2=size=1920x1080:rate=30' \
  -vf 'format=nv12,hwupload' -c:v h264_vaapi -profile:v high \
  -b:v 8M -bf 0 -g 30 -frames:v 300 -f null -
```

## Tested transcode configuration

Use *only* for H.264 VAAPI decode directly into VAAPI encode of the same frames, without intermediary filters/consumers that read decoder NV12 surfaces. Direct Convert defers decoder linearization; it is not general-purpose zero-copy.

```sh
LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 \
LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1 \
LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1 \
LIBVA_V4L2_TRANSCODE_PADDING_ONLY=1 \
LIBVA_V4L2_TRANSCODE_TILE_GROUP=1 \
LIBVA_V4L2_TRANSCODE_MEMORY_MODE=direct \
LIBVA_V4L2_TRANSCODE_DETILE_ORDER=tile \
LIBVA_V4L2_TRANSCODE_PREFETCH=0 \
ffmpeg -hide_banner -loglevel warning -nostats \
  -hwaccel vaapi -hwaccel_output_format vaapi \
  -vaapi_device /dev/dri/renderD128 -i /path/to/h264-input.mp4 \
  -c:v h264_vaapi -profile:v constrained_baseline \
  -b:v 5M -bf 0 -g 30 -frames:v 300 -f null -
```

Environment variables are **opt-in**; this release does not silently enable Direct Convert by default. `LIBVA_V4L2_ENC_DEBUG=0` or unset disables encoder diagnostics; `=1` enables them. See [`MT8183-CONFIGURATION.md`](MT8183-CONFIGURATION.md).

## Limitations / compatibility

- `VAProfileH264ConstrainedBaseline` is mapped to V4L2 Baseline because the MT8183 encoder accepts Baseline, not Constrained Baseline. The observed SPS has `profile_idc=66`, `constraint_set0_flag=0`, `constraint_set1_flag=0`: output is signaled as **Baseline**, not Constrained Baseline.
- Packed headers are not advertised; FFmpeg may print packed/global-header warnings. A tested H.264 High MP4 reported `extradata_size=36`, and decoded with FFmpeg exit code 0. This does not prove every container/player will accept every output.
- MDP3 conversion remains unavailable; CPU MM21 detiling is necessary. The direct-convert path avoids a *second* linear NV12 copy, not CPU involvement.
- Other SoCs, profiles, resolutions, applications, errors injected into ioctl calls, and 720p crop reporting have not been exhaustively validated.
- No kernel or firmware changes are bundled.

## Validation

See [`REGRESSION.md`](REGRESSION.md) for completed on-device testing and the final RC1 acceptance checklist. `test/rc1-regression.sh` executes basic tests without leaving large MP4 files in `/tmp`.

Historical experimental notes and full documentation are retained in this tree.
