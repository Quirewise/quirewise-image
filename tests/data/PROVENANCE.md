# Test data provenance and licenses

This directory contains only fixtures. None of them are installed, exported or
shipped in the release packages; they exist for `ctest`.

## `jp2/t803_*` — ITU-T T.803 conformance attachment

Extracted byte-for-byte from the official ITU-T T.803 (V3, 02/2024) conformance
attachment (see `T803_PROFILE0_NOTICE.md` for the exact file mapping).
Copyright notice: `T803_PROFILE0_COPYRIGHT.txt`. The rightsholders (Kakadu
Software, Algo Vision Technology, Aware, Kodak, Ricoh) grant use of these files
for testing conformance to the JPEG 2000 standard, require this notice to be
kept in copies, and grant no rights for non-JPEG-2000 uses. Quirewise uses them
only in JPEG 2000 decoder tests.

## `jp2/synthetic_*`, `jp2/gray97_*`, `jp2/rgb_u1*`, `jp2/gray53_*`

Generated during Quirewise development from synthetic pixel data and encoded
with OpenJPEG 2.5.4 (encoder used as a tool; no OpenJPEG code is vendored or
linked). Reference `.gray`/`.gray8` planes were produced by the same generation
scripts. They contain no third-party imagery.

## `tiff/` and `webp/`

Small synthetic fixtures (8×8 TIFF/JPEG, 31×17 and animation WebP) generated for
the TIFF/JPEG and WebP decoder tests. They contain no third-party imagery.

## Removed

`jp2/balloon.jp2` and `jp2/balloon.j2c` were removed in `M4-10`: no test, script
or document referenced them, and their origin could not be established.
