# RC1 regression record and acceptance checklist

## Tests completed on MT8183 **before assembling this RC1 archive**

| Test | Result |
|---|---|
| Audit 01: 3300 frames, `testsrc2` 1080p H.264 High, debug=0/1 | Byte-identical MP4 SHA-256 `6863e87361cc8bbe2da934efc1afee3fcfd4abd471e70a4bf0cdabbe945b622a` |
| MP4 inspection and decode | H.264 High 1920×1080, 3300 frames, `extradata_size=36`, FFmpeg decode exit=0 |
| Audit 02: 300 frames MMAP vs DMA-BUF staging | Both MP4 SHA-256 `be39139f5bc8039b1c75e8b22b8a1660c85d1af94de7c8ee727c142cd760f632`; `mmap=300` vs `dmabuf_staging=300` |
| Audit 02: 3300-frame High→Baseline transcode | `frames=3300`, `dmabuf_staging=3300`, `direct-convert fallbacks=0`; output decode exit=0 |
| Audit 03: 300-frame MMAP vs DMA-BUF staging, `-f null` | `mmap=300` and `dmabuf_staging=300`, both FFmpeg exit=0, fallback/rejection counters 0 |
| Baseline SPS | `profile_idc=66`, constraint flags 0 (signals Baseline, not Constrained Baseline) |

**Important:** the tests above validate Audit 01–03 source versions, not a completed RC1 final-device regression. The release archive also changes only Meson version metadata and documentation; it still needs its own on-device acceptance run.

## RC1 acceptance criteria

1. Build extracted source via Meson + Ninja, without Git checkout.
2. Run `test/rc1-regression.sh` (300 frames MMAP + 300 DMA-BUF staging; optional 300-frame transcode) and require exit 0.
3. Verify `VENC input statistics` counters match expected paths and that `TRANSCODE DIRECT CONVERT stats` reports `fallbacks=0` in optional transcode mode.
4. Perform a separate output MP4 decode + SPS check if container compatibility is part of a deployment requirement; `-f null` alone does not prove container metadata correctness.
5. Keep the known limitations listed in README and avoid promoting untested platforms to stable support.

**Not verified:** deliberate ioctl failure injection, partially initialized queues under synthetic errors, broad non-MT8183 coverage. These remain known validation gaps.
