# MT8183 configuration (v0.1.0-rc1)

| Variable | Recommended value | Description |
|---|---|---|
| `LIBVA_DRIVERS_PATH` | `$PWD/build/src` | Directory containing `v4l2_drv_video.so` |
| `LIBVA_DRIVER_NAME` | `v4l2` | Force this libva driver |
| `LIBVA_V4L2_ENC_DEBUG` | unset / `0` | `1` enables verbose encoder diagnostics and summary stats |
| `LIBVA_V4L2_ENC_DMABUF` | `1` for transcode | Opt-in VENC DMA-BUF input; `0`/unset uses MMAP |
| `LIBVA_V4L2_ENC_DIRECT` | `1` for known transcode setup | Optional direct PRIME eligibility; does not guarantee `direct_prime` |
| `LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT` | `1` **only** for VAAPI decode→encode without intermediate readers | Delay decoded MM21 conversion and detile into VENC DMA-BUF |
| `LIBVA_V4L2_TRANSCODE_PADDING_ONLY` | `1` with direct convert | Avoid clearing bytes overwritten by MM21 detiler |
| `LIBVA_V4L2_TRANSCODE_TILE_GROUP` | `1` | Keep tested single-tile processing; `4`/`8` experimental |
| `LIBVA_V4L2_TRANSCODE_MEMORY_MODE` | `direct` | `ramcopy` is diagnostic and slower in tests |
| `LIBVA_V4L2_TRANSCODE_DETILE_ORDER` | `tile` | `row` is experimental and slower in tests |
| `LIBVA_V4L2_TRANSCODE_PREFETCH` | `0` | `1`/`2` experimental; long runs show no reliable improvement |
| `LIBVA_V4L2_DMA_HEAP` | unset | Override allocator source when needed |
| `LIBVA_V4L2_BUFFERS` | unset | Decoder queue buffer count; see upstream README |
| `LIBVA_V4L2_MDP3_PATH` | unset | Optional MDP3 device selection; MDP3 broken on tested system |

**Defaults are not all the same as recommended values.** For example, Direct Convert and padding-only are opt-in. Use explicit environment settings for the validated transcode workload. Setting `LIBVA_V4L2_TRANSCODE_DIRECT_CONVERT=1` may be unsafe with `vaGetImage`, `vaDeriveImage`, PRIME export, CPU filters, or intermediate consumers of the decoder surface.

`LIBVA_V4L2_ENC_DEBUG` interprets unset, empty, and `0` as off. Other variables are parsed independently; not every variable has general-purpose true/false semantics. See their experiment notes for details.
