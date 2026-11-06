# JP2/J2K profile support matrix

Matrix version: 1 (2026-11-06), decoder commit `libraries/quirewise-image` `f7c51db` (M4-10 provenance head) + M4-25 profile test.

Scope: the finite profiles below are the only inputs declared as
supported. Any JP2/J2K input outside this table must return the stable
`unsupported` status without publishing partial output.

Comparison contract: size, component count, pixel format and error
class must match exactly. Lossless/reversible rows are checked
byte-for-byte against the committed golden. Rows using the 9/7
irreversible transform are allowed at most 1 LSB per channel against
the pinned OpenJPEG 2.5.4 reference bytes; the observed maximum is
recorded in the table and is 0 for every reference row.

Test: `quirewise_image_jp2_profile_matrix` (`tests/jp2_profile_matrix_test.c`). Fixture hashes are SHA-256.

| Container | Components / precision / signed | Tile / progression | Transform / MCT | Colour / CDEF / alpha | Resource limits | Fixture SHA-256 (16 hex) | Expected status | Golden / tolerance |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| JP2 | 1 / 8-bit / unsigned | single tile / LRCP | 9/7 irreversible | grayscale | default image limits | `469d486e50243850` | OK | 32x24 gray8; max-diff 0 |
| JP2 | 1 / 8-bit / unsigned | single tile / LRCP | 9/7 irreversible, rate 20 | grayscale | default image limits | `bdffe7d19f34ff7c` | OK | 32x24 gray8; max-diff 0 |
| J2K | 1 / 8-bit / unsigned | 8x8 tiles / LRCP | 9/7 irreversible | grayscale | default image limits | `2886d26503987a64` | OK | 16x16 gray8; max-diff 0 |
| J2K | 1 / 8-bit / unsigned | 8x8 tiles / LRCP | 9/7 irreversible, ROI shift 2 | grayscale | default image limits | `c94d8b0a358aa349` | OK | 16x16 gray8; max-diff 0 |
| J2K | 1 / 8-bit / unsigned | 8x8 tiles / LRCP | 9/7 irreversible, ROI shift 4 | grayscale | default image limits | `f05aad4a51ee3f17` | OK | 16x16 gray8; max-diff 0 |
| J2K | 1 / 8-bit / unsigned | 8x8 tiles, image origin 2x2 / LRCP | 9/7 irreversible | grayscale | default image limits | `76dafd51d8181e07` | OK | 20x14 gray8; max-diff 0 |
| J2K | 1 / 8-bit / unsigned | 8x8 tiles / LRCP | 5/3 reversible, dx=2 dy=1 | grayscale | default image limits | `e336e849e566bfb6` | OK | 8x16 gray8; max-diff 0 |
| J2K | 1 / 8-bit / unsigned | 8x8 tiles / LRCP | 5/3 reversible, dx=2 dy=2 | grayscale | default image limits | `5557355bcb130124` | OK | 8x8 gray8; max-diff 0 |
| J2K | 1 / 8-bit / unsigned | 8x8 tiles / LRCP | 5/3 reversible, dx=2 dy=1 | grayscale | default image limits | `f5bdc8e2f252d1ce` | OK | 10x14 gray8; max-diff 0 |
| JP2 | 1 / 8-bit / unsigned | single tile / LRCP | 5/3 reversible | grayscale | default image limits | `ab9cb2461110fed3` | OK | 13x17 gray8; shape + status only |
| JP2 | 3 / 8-bit / unsigned | single tile / LRCP | 9/7 irreversible, ICT | RGB | default image limits | `1e3225d185f364db` | OK | 16x16 rgb24; shape + status only |
| JP2 | 3 / 8-bit / unsigned | single tile / LRCP | 5/3 reversible, RCT | RGB | default image limits | `66fae2adba987b18` | OK | 16x16 rgb24; shape + status only |
| JP2 | 4 / 8-bit / unsigned | single tile / LRCP | 9/7 irreversible | RGB + straight alpha, CDEF | default image limits | `a4682c5c97e4c128` | OK | 16x16 rgb24; shape + status only |
| JP2 | 4 / 8-bit / unsigned | single tile / LRCP | 9/7 irreversible | RGB + premultiplied alpha, CDEF component 2 | default image limits | `5c46e357e8ac9118` | OK | 16x16 rgb24; shape + status only |
| JP2 | 1 / 8-bit indexed palette | single tile / LRCP | 5/3 reversible | PCLR/CMAP gray palette | default image limits | `a5e926fbe9df3021` | OK | 13x17 gray8; shape + status only |
| JP2 | 3 / 8-bit / unsigned | single tile / LRCP | 9/7 irreversible, no MCT, 4:2:0 | RGB | default image limits | `fbdbf3766f1298d5` | OK | 16x16 rgb24; shape + status only |
| J2K (T.803 Profile 0) | 1 / 8-bit / unsigned | single tile / LRCP | 5/3 reversible | grayscale | default image limits | `409c62a227497e7f` | OK | 17x37 gray8; shape + status only |

Reference files (committed golden bytes):

- `gray97_openjpeg_32x24.jp2` → `gray97_openjpeg_32x24.gray` (SHA-256 `3955ec892c728c39`)
- `gray97_openjpeg_rate20_32x24.jp2` → `gray97_openjpeg_rate20_32x24.gray` (SHA-256 `df3471dfc6f9f86d`)
- `synthetic_gray8_16x16_tiles_8x8_97_no_roi.j2k` → `synthetic_gray8_16x16_tiles_8x8_97_no_roi_reference.gray8` (SHA-256 `c66394e4f9fdc25e`)
- `synthetic_gray8_16x16_tiles_8x8_97_roi_u2.j2k` → `synthetic_gray8_16x16_tiles_8x8_97_roi_u2_reference.gray8` (SHA-256 `c66394e4f9fdc25e`)
- `synthetic_gray8_16x16_tiles_8x8_97_roi_u4.j2k` → `synthetic_gray8_16x16_tiles_8x8_97_roi_u4_reference.gray8` (SHA-256 `c66394e4f9fdc25e`)
- `synthetic_gray8_origin2x2_20x14_tiles8x8_97.j2k` → `synthetic_gray8_origin2x2_20x14_tiles8x8_97_reference.gray8` (SHA-256 `879c3256f74a3dcc`)
- `synthetic_gray8_subsampled_16x16_dx2x1_tiles8x8.j2k` → `synthetic_gray8_subsampled_16x16_dx2x1_reference.gray8` (SHA-256 `3f3e95485f033e73`)
- `synthetic_gray8_subsampled_16x16_dx2x2_tiles8x8.j2k` → `synthetic_gray8_subsampled_16x16_dx2x2_reference.gray8` (SHA-256 `32061d3e3b547e02`)
- `synthetic_gray8_subsampled_20x14_dx2x1_tiles8x8.j2k` → `synthetic_gray8_subsampled_20x14_dx2x1_reference.gray8` (SHA-256 `cada1b32b180acfb`)

Unlisted profiles (for example PCLR entries with precision above 8
bits, unsupported colour spaces, or unsupported codec features) are
rejected by the structure validator with the documented
`unsupported` status.

