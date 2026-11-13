# MT8183 encoder: discovery and bitrate diagnostics

Build: `ninja -C build`

Auto detection: `LIBVA_DRIVERS_PATH="$PWD/build/src" LIBVA_DRIVER_NAME=v4l2 vainfo`.
Expect H.264 decode VLD and encode EncSlice (if both nodes are accessible).

Bitrate diagnosis: Run an encoding job with
`LIBVA_V4L2_ENC_DEBUG=1` in the environment, with and without
`LIBVA_V4L2_VIDEO_PATH=/dev/video2`. You should see
`H264 VA RC misc bits_per_second=...` followed by
`H264 requested bitrate=... V4L2 readback=...`.

For a reproducible comparison, use the same `testsrc2` input, dimensions,
framerate and frame count for 1M, 4M and 10M. Compare file sizes and SHA256.
If VA RC and V4L2 readback change as requested yet the output remains
identical, the next check is firmware/kernel handling of rate-control,
not just the VA-API control.

This patch does not claim to fix bitrate regulation itself.
