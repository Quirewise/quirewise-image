# quirewise-image

Standalone C11 image library: still-image probing and decoding, image-folder
enumeration, and bounded grayscale scan processing. It can be consumed without
the Quirewise application repository.

CMake target: `Quirewise::Image`. Public headers are under `include/quirewise/`.

## Input formats

`qw_image_probe` / `qw_image_decode` cover JPEG (baseline and progressive),
PNG (including APNG/MNG default images), GIF87a/89a (first frame), BMP, TIFF
(top-level IFDs), WebP (VP8/VP8L/alpha/animation), JP2/J2K, PSD/PSB flattened
composites, OpenEXR scanline images, Radiance HDR, Softimage PIC, TGA, PCX,
PFM, DDS (BC1/DXT1), QOI, XPM/XBM/WBMP, PNM/PAM, ICO/CUR, ICNS, SGI, Sun
Raster, MIFF, DICOM (single-frame uncompressed) and XCF.

## Not implemented

- AVIF, HEIC/HEIF, JPEG XL, PCD and camera RAW (DNG/TIFF-CFA, CR3, RAF, X3F,
  CRW, MRW) can be probed but decoders return `QW_IMAGE_ERR_UNSUPPORTED`. The
  package does not link LibRaw or any external codec.
- There is **no public image encoder**: the API is probe/decode/folder/scan
  processing only. PNG output in the application comes from the document
  package's internal encoder for the PDF-to-PNG route.
- Multi-frame GIF/WebP/TIFF/ICO/ICNS are decoded per frame/entry by the internal
  page decoders; the public API returns one image per call.

## Limits

Decoding enforces per-image dimension, channel, frame-count and byte limits
(default 20 megapixels) and returns `QW_IMAGE_ERR_RESOURCE_LIMIT` on excess.
Transparent pixels are composited on white; EXIF/ICC metadata is not preserved.

## Build

```powershell
cmake -S . -B build
cmake --build build
cmake --install build --prefix install
```

The umbrella target is `Quirewise::Image`.
