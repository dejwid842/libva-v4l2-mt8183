# Fix: stable FFmpeg hwupload method selection

FFmpeg 7.1.5 can call `vaDeriveImage()` before `vaCreateConfig()` for its
NV12 hwupload pool. If the first generic derive succeeds, later rejecting
`vaDeriveImage()` after the encoder configuration appears fails the filter
with `Failed to derive image ... 20` / `Failed to upload frame: -5`.

The backend now remembers a successful *generic* `vaDeriveImage()` probe
per VA driver instance and does not switch later generic surfaces to
`vaPutImage()` merely because an encoding config has since appeared.
Explicit encoder usage hints still select Direct PRIME upload; the
existing `LIBVA_V4L2_ENC_FORCE_PUTIMAGE=1` diagnostic opt-in also remains.

## Modes / expectations

- `LIBVA_V4L2_ENC_DMABUF=1 LIBVA_V4L2_ENC_DIRECT=1` **without** FORCE:
  encoder should finish without `hwupload` error; FFmpeg 7.1.5 with generic
  surfaces is expected to use `dmabuf_staging`, not `direct_prime`.
- Add `LIBVA_V4L2_ENC_FORCE_PUTIMAGE=1` only for encoder-only experiments:
  previous Direct PRIME behavior (`direct_prime=300`) remains available.
- Decoder use should not be changed by this fix. Please test actual
  decode+encode independently before enabling the mode by default.

This is a regression fix, not a general solution to transparent zero-copy:
without an early ENCODER hint, VA-API cannot always identify the future
purpose of a generic surface before an encoder configuration exists.
