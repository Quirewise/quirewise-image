# WebP decoder security review (DEP-011)

- Date: 2026-11-06
- Scope: `libraries/quirewise-image/src/formats/image/webp_parser.c`,
  `webp_vp8.c`, `webp_vp8_reconstruct.c`, `webp_lossless.c`
- Decoder version: image repository `2bd4be9` (M4-25 head)
- Reviewer: automated review + coverage-guided fuzz campaign (see below)

## Trust boundary

All WebP input is untrusted. The decoder is called through
`qw_image_decode()` and returns either a fully decoded `QwImage` or a stable
error code; partial pixel buffers are never published.

## Input validation

| Layer | Checks |
| --- | --- |
| RIFF container | `RIFF` magic, `WEBP`, even RIFF size, `riff_size >= 4`, each chunk bounded by `chunk_size <= end - payload` |
| Chunk probing | `VP8 ` requires the 3-byte start code and key-frame bit; `VP8L` requires signature `0x2F`; `VP8X` requires 10-byte payload; `ALPH` requires 16-byte header |
| VP8 frame header | 10-byte minimum, `partition_size >= 2`, `partition_size <= size - 10`, width/height limits before allocation |
| Boolean/coefficient/token decoding | bit reader tracks `failed`; partition offset arithmetic uses checked additions; token tree sizes must be even and within the partition |
| VP8L | VLQ capped at 5 bytes, code lengths ≤ 15, groups ≤ 1024, cache bits ≤ 11, back-reference length ≤ 4096, transform/prefix sizes bounded |
| Animation | `ANIM`/`ANMF` presence ordering, `frame_count <= QW_WEBP_MAX_FRAMES`, canvas dimensions limited before allocation |
| Alpha | `ALPH` header size, compression method and filter validated before use |

## Resource limits and overflow safety

- `qw_image_dimensions_within_limits()` guards every allocation path in
  `webp_lossless.c` and the VP8 frame header.
- Pixel counts are checked against `SIZE_MAX / bpp` and `SIZE_MAX / 4` before
  multiplication, so 32-bit and 64-bit hosts cannot wrap.
- Frame tables are capped by `QW_WEBP_MAX_FRAMES`; VP8L group tables are capped
  by `WEBP_LOSSLESS_MAX_GROUPS`.
- No `malloc` size depends on an unchecked 64-bit field; every size is narrowed
  only after a bounds check.

## Failure semantics

- Format errors return `QW_IMAGE_ERR_FORMAT`; unsupported features return
  `QW_IMAGE_ERR_UNSUPPORTED`; limit violations return
  `QW_IMAGE_ERR_RESOURCE_LIMIT`.
- Bit readers set a sticky `failed` flag; callers stop and free the partially
  built image before returning.
- Animation decode counts frames first and allocates only when the count is
  within limits.

## Fuzz evidence

- Tool: `quirewise_image_jp2_public_dispatch_fuzzer` built with
  `-fsanitize=address,undefined` and GCC `-fsanitize-coverage=trace-pc`.
- Corpus: all 41 committed WebP vectors (`tests/data/webp/*.webp`), including
  lossy VP8, VP8L lossless, alpha, multi-partition, loop-filter and animation.
- Campaign: 10,000 iterations, fixed RNG seed `0x51574a32`, max input 1 MiB,
  128-slot corpus.
- Result: `structure_valid=0`, `decode_success=2267`,
  `decode_unsupported=2726`, `decode_other=5048`, `coverage_slots=2552`,
  exiting cleanly with **no ASan/UBSan diagnostics**.
- Log: `.tmp/quirewise-agent-evidence/M4-26/webp-fuzz.log`.

## Residual risk

- The fuzz campaign is time-bounded; it is evidence of robustness, not a proof.
- Visual quality comparisons remain covered by the fixed libwebp 1.6.0 RGB
  references in `tests/data/webp` (M2-01), not by this review.

## Verdict

The decoder validates container, frame, transform and allocation inputs before
use, fails transactionally with distinct status codes, and survived the
coverage-guided sanitizer campaign. No open defect was found in this review.
