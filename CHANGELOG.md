# Changelog

## 0.1.0-rc1 — 2026-11-08

**Scope:** tested MT8183 H.264 encode/decode/transcode workflow. This is an RC, not a blanket VA-API compatibility claim.

- Consolidated tested VENC MMAP and DMA-BUF staging, H.264 hardware encode and MM21 CPU detile into NV12M VENC staging for transcoding.
- Audit 01: normalized `LIBVA_V4L2_ENC_DEBUG=0` / `=1` semantics across encoder diagnostic sites.
- Audit 02: explicit checks for DMA-BUF CPU-sync END before submission and noexcept cleanup behavior.
- Audit 03: release partially allocated V4L2 queues when encoder construction fails in final queue setup.
- Kept experimental row-order, ramcopy, prefetch, and tile grouping behind opt-in environment variables, without enabling them by default.
- Documentation-only release consolidation and fixed Meson project version `0.1.0-rc1` for Git-independent source archives.

**Known issues:** MDP3 `MDP_IPI_EINVAL`, Constrained Baseline request produces SPS-signaled Baseline, lack of packed-header advertisement, limited device and format coverage. Fault injection for ioctl failures is not yet validated on hardware.
