# ITU-T T.803 Profile 0 test fixtures

These files are extracted byte-for-byte from the official ITU-T T.803 (V3, 02/2024) conformance attachment:

- `t803_profile0_p0_01.j2k`: `files/codestreams_profile0/p0_01.j2k`
- `t803_profile0_p0_01_class0.pgx`: `files/reference_class0_profile0/c0p0_01.pgx`
- `t803_profile0_p0_09.j2k`: `files/codestreams_profile0/p0_09.j2k`
- `t803_profile0_p0_09_class0.pgx`: `files/reference_class0_profile0/c0p0_09.pgx`
- `t803_profile0_p0_09_class1.pgx`: `files/reference_class1_profile0/c1p0_09-0.pgx`
- `t803_profile0_p0_02.j2k`: `files/codestreams_profile0/p0_02.j2k`
- `t803_profile0_p0_02_class1.pgx`: `files/reference_class1_profile0/c1p0_02-0.pgx`
- `t803_profile0_p0_11.j2k`: `files/codestreams_profile0/p0_11.j2k`
- `t803_profile0_p0_11_class0.pgx`: `files/reference_class0_profile0/c0p0_11.pgx`
- `t803_profile0_p0_12.j2k`: `files/codestreams_profile0/p0_12.j2k`
- `t803_profile0_p0_12_class0.pgx`: `files/reference_class0_profile0/c0p0_12.pgx`
- `t803_profile0_p0_03.j2k`: `files/codestreams_profile0/p0_03.j2k`
- `t803_profile0_p0_03_class1.pgx`: `files/reference_class1_profile0/c1p0_03-0.pgx`
- `t803_profile0_p0_08.j2k`: `files/codestreams_profile0/p0_08.j2k`

The p0_09 vector is a single unsigned 8-bit grayscale component, 17x37, one zero-origin tile, irreversible 9-7, five decomposition levels, and scalar-expounded QCD. The T.803 v3 Table C.6 Class 1 Profile-0 reference requires full-resolution output with peak error 0 and MSE 0. The Table C.1 Class 0 Profile-0 reference exercises resolution reduction 2, producing 5x10 output with maximum peak error 4 and MSE 1.47.

The internal decoder tests also derive runtime-only p0_09 marker variants for main QCC, tile QCD, and tile QCC. They change quantization step mantissas to exercise effective-marker selection and compare the resulting outputs across variants; these mutations are synthetic decoder tests, not additional official conformance vectors.

The internal decoder tests also derive a runtime-only aligned nonzero-origin variant of p0_09. The test translates the image and tile origins by 2^(5+15) reference-grid units (1,048,576), leaving tile dimensions and packet data unchanged. Direct output matches all 629 samples in the unchanged official Class 1 reference. This is a synthetic one-tile geometry regression, not another official vector or evidence for multi-tile origin handling.

The `synthetic_gray8_16x8_tiles_8x8.j2k` and `synthetic_gray8_16x16_tiles_8x8.j2k` files are synthetic reversible 5-3 grayscale vectors encoded from generated P5 images with OpenJPEG 2.5.4. Their 8x8 tiles use two decomposition levels; direct internal decode and normal dispatch match every source pixel for the 2x1 and 2x2 tile grids. The `synthetic_gray8_crop4x0_16x8_tiles_8x8.j2k` vector uses the OpenJPEG image offset `-d 4,0` with a zero tile offset and a 3x1 tile grid; output matches all 128 samples in the visible 16x8 image region. The `synthetic_gray8_origin2x2_20x14_tiles8x8.j2k` vector uses `-d 2,2`, a zero tile offset, a 3x2 grid of 8x8 tiles, and two decomposition levels; direct internal decode and normal dispatch match all 280 source pixels. The corresponding irreversible 9-7 vector matches all 280 OpenJPEG 2.5.4 reference pixels, and a runtime tile-QCD override on the first tile matches its separately decoded reference. These are decoder regression fixtures, not official T.803 vectors. The exercised tile-local path supports full-resolution unsigned grayscale with unit sampling and aligned 2x1 or 2x2 component sampling on a 16x16 image with 8x8 tiles, plus non-aligned origins and wavelet phases for unit sampling and per-tile QCD state. General edge-grid handling for subsampled components remains incomplete: an OpenJPEG 2.5.4 20x14 dx2x1 probe with a 3x2 tile grid returns `QW_STATUS_ERR_FORMAT` during structural validation at tile 2's packet header (packet 2 of 3; packet parser rc=-2). Tile COD/COC, ROI, reduced-resolution output, and multi-component MCT/color/alpha remain unsupported. A 16x16 probe with image origin (2,2) and 8x8 tiles gives tile 7 an 8x2 terminal extent; packet traversal accounts for 14 of its 33 packet bytes and rejects the remaining 19 bytes with a format error. This small terminal-tile geometry remains outside the demonstrated support.

The p0_02 vector is unsigned 8-bit grayscale with 2x1 component sampling, a 127x126 reference grid, one zero-origin tile, six LRCP quality layers, and three decomposition levels. The main COD declares irreversible 9-7, but its component COC overrides the effective transform to reversible 5-3 and sets 32x32 code-blocks, TERMALL, predictable termination, and segmentation symbols; SOP and EPH are enabled. Its Class 1 Profile-0 reference is a 64x126 component sample plane; Table C.6 requires peak error 0 and MSE 0.

The p0_11 vector is a 128x1 unsigned 8-bit grayscale image with one tile, one LRCP layer, zero decomposition levels, reversible 5-3, 64x64 code-blocks, and a signaled 128x2 precinct (PPx=7, PPy=1). It enables EPH and segmentation symbols. Structural validation, direct internal decode, and normal dispatch all pass; the direct and dispatched outputs match all 128 official Class 0 PGX samples exactly.

The p0_12 vector is a 3x5 unsigned 8-bit grayscale image with one tile, one LRCP layer, three decomposition levels, reversible 5-3, and 32x32 code-blocks. It enables SOP and TERMALL. Structural validation, direct internal decode, and normal dispatch all pass; the direct and dispatched outputs match all 15 official Class 0 PGX samples exactly.

The p0_03 vector is a signed 4-bit 256x256 grayscale image split across four 128x128 tiles. It exercises POC, a component QCC override, component registration, TLM, and an implicit ROI shift of 7. The official Class 1 full-resolution PGX reference is 256x256 and requires zero peak error and zero MSE. Structural validation now accounts for the implicit ROI shift when checking packet zero-bitplane bounds. The public dispatcher decodes the vector through OpenJPEG and matches all 65,536 official Class 1 samples after mapping each signed sample s in [-8, 7] to Gray8 as round((s + 8) * 255 / 15). Direct internal decoding remains unsupported because the current path requires unsigned 8-bit samples and does not implement inverse ROI handling. The p0_15 codestream and Class 1 reference are byte-identical aliases of p0_03, so they are not duplicated as separate fixtures.

The p0_08 vector is an unsigned 8-bit three-component codestream with 30 CPRL layers. Its COC markers set 6, 7, and 8 decomposition levels for components 0, 1, and 2; no-quantization QCC markers for components 0 and 2 have lengths derived from their component-specific decomposition levels.

The p0_16 codestream is stored in T.803's Profile 0 codestream collection, while its syntax description requires JPEG 2000 full-standard capabilities. It is a 128x128 unsigned 8-bit grayscale image with one tile, three RLCP layers, three decomposition levels, reversible 5-3, and 64x64 code-blocks. Structural validation, direct internal decode, and normal dispatch all pass; the direct and dispatched outputs match all 16384 official Class 0 PGX samples exactly.

Official source archive:
https://www.itu.int/wftp3/public/t/testsignal/SpeImage/T803/v2024_02/T.803v3_15444-4ed4-ElecAtt-codestreams.zip

Archive SHA-256:
`ac04b52e1fe38404912036c14f215099ea9a785f38644fbe76ae8f3d1523c86d`

`T803_PROFILE0_COPYRIGHT.txt` retains the copyright statement included in the official archive. The PGX files retain their original headers. No broader license claim is made here.

The p0_09 Class 0 PGX fixture SHA-256 is `CE4DCF09FBD8076527F8D3EA83D9A6C23E24C8AA50F74AC7CD943695F59C875A`.
