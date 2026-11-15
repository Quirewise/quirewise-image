# quirewise-image

Still-image probing and decoding, non-recursive image-folder enumeration, and
bounded grayscale scan cleanup for Quirewise. No external codec is linked.

`Quirewise::Image` is an independent, MIT-licensed C11 library. It is installed
as a CMake package (`QuirewiseImage`) and can be consumed without the Quirewise
application repository.

## What it does

- Probes a memory buffer for a supported raster signature.
- Decodes one still image to owned Gray8 or RGB24 pixels.
- Recognises image filenames by extension and lists a directory of images in
  natural filename order.
- Cleans up scanned grayscale pages: illumination normalisation, deskew, crop,
  perspective correction, dewarp, and double-page split.
- Encodes Gray8 or RGB24 pixels as PNG, BMP or baseline TIFF.

Decoding covers JPEG (baseline and progressive), PNG (APNG decodes its default
image), MNG, GIF87a/89a (first frame), BMP, TIFF (first top-level IFD; none,
CCITT T.6, LZW, JPEG, Deflate and PackBits), WebP (VP8/VP8L/alpha, still only),
JP2/J2K, PSD/PSB flattened composites, OpenEXR scanline images, Radiance HDR,
Softimage PIC, TGA, PCX, PFM, DDS (DXT1/BC1), QOI, XPM, XBM, WBMP, PNM/PAM,
ICO/CUR (first entry), SGI, Sun Raster, MIFF, DICOM (single-frame uncompressed)
and XCF. The write side currently covers PNG (stored zlib), BMP (24-bit) and
baseline TIFF (uncompressed); JPEG and WebP encoding are not implemented yet.
It does **not** decode AVIF/HEIC/JPEG XL/camera RAW, decode animated WebP, or
preserve EXIF/ICC metadata.

## Requirements

- C11 compiler and CMake 3.21 or newer.
- Installed `QuirewiseRuntime` and `QuirewiseCompression` packages found with
  `find_package`; no third-party libraries. CTest drives the test suite.

## Build, test and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Shared builds are available with `-DQUIREWISE_BUILD_SHARED=ON`.

## Use it from CMake

```cmake
find_package(QuirewiseImage CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Image)
```

`Quirewise::Image` is an interface bundle over the decode, folder and processing
targets, so one link line is enough; add `-DCMAKE_PREFIX_PATH=/path/to/prefix`
when the package is installed elsewhere.

## Quick start

```c
#include <quirewise/image.h>

#include <stdio.h>
#include <stdlib.h>

static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    long length;

    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_size = (size_t)length;
    return data;
}

int main(int argc, char **argv)
{
    QwImage image = {0};
    size_t size = 0;
    unsigned char *data;
    int status;

    if (argc != 2 || !qw_image_name_is_supported(argv[1])) return 1;
    data = read_file(argv[1], &size);
    if (!data || !qw_image_probe(data, size)) {
        free(data);
        return 1;
    }
    status = qw_image_decode(data, size, &image);
    free(data);
    if (status != QW_IMAGE_OK) {
        fprintf(stderr, "%s\n", qw_image_status_string(status));
        return 1;
    }
    printf("%ux%u %s stride=%zu\n", image.width, image.height,
           image.format == QW_IMAGE_PIXEL_GRAY8 ? "gray8" : "rgb24",
           image.stride);
    qw_image_free(&image);
    return 0;
}
```

## API overview

| Header | Purpose |
|---|---|
| `quirewise/image.h` | probe, decode, encode, status text, ABI version |
| `quirewise/image_folder.h` | non-recursive image-directory listing |
| `quirewise/image_processing.h` | Gray8 scan cleanup and page split |

| Task | Functions |
|---|---|
| Probe / decode | `qw_image_probe`, `qw_image_decode`, `qw_image_free` |
| Encode | `qw_image_encode`, `qw_image_buffer_free` |
| Filename check | `qw_image_name_is_supported` |
| Folder listing | `qw_image_folder_open`, `qw_image_folder_free`, `qw_image_folder_is_directory` |
| Gray8 ownership | `qw_gray_image_copy`, `qw_gray_image_adopt`, `qw_gray_image_release` |
| Scan cleanup | `qw_gray_image_normalize_illumination`, `qw_gray_image_deskew`, `qw_gray_image_crop`, `qw_gray_image_correct_perspective`, `qw_gray_image_dewarp` |
| Page split | `qw_gray_image_split_copy` |
| Diagnostics | `qw_image_abi_version`, `qw_image_status_string`, `qw_image_processing_abi_version`, `qw_image_processing_status_string`, `qw_image_folder_abi_version`, `qw_image_folder_status_string` |

## Behavior and limits

- `qw_image_decode` returns `QW_IMAGE_OK` (0) or a negative `QwImageStatus`.
  `qw_image_probe` returns `1`/`0` only; a successful probe does not guarantee a
  successful decode.
- Output is tightly packed Gray8 (stride == width) or RGB24 (stride == 3 *
  width). Alpha is composited on white; palette and 16-bit sources are reduced
  to 8-bit Gray8/RGB24.
- `QwImage.pixels` belongs to the value and is released with `qw_image_free`.
  Input bytes are borrowed and may be freed after the call returns.
- `qw_image_encode` writes PNG/BMP/TIFF and returns a malloc-owned buffer that
  is released with `qw_image_buffer_free`. JPEG and WebP return
  `QW_IMAGE_ERR_UNSUPPORTED`; no resampling, color conversion or metadata is
  synthesized, and the stride may exceed the packed row size.
- Decoding enforces a 32768-pixel maximum dimension and 20,000,000 decoded
  pixels, returning `QW_IMAGE_ERR_RESOURCE_LIMIT`.
- `qw_image_folder_open` is non-recursive and keeps only names accepted by
  `qw_image_name_is_supported`, sorted naturally. A zero option field selects
  the documented default: 10,000 entries, 1 GiB total, 256 MiB per file. Paths
  are UTF-8 on Windows and native bytes on POSIX; entries belong to the folder.
- `QwGrayImage` operations modify the image in place and may replace or resize
  the pixel buffer; on failure the value stays releasable but may be modified.
  Processing enforces the same 32768-pixel dimension and 20,000,000-pixel caps.
- Animated WebP, AVIF, HEIC, JPEG XL, PCD and camera RAW have no decoder.
  RAW/DNG signatures can still satisfy `qw_image_probe`; decoding reports
  `QW_IMAGE_ERR_UNSUPPORTED`. `.icns` is a recognised extension without a
  public decoder, so folder listings can contain files decode rejects.

## Testing

`ctest` runs JP2 packet header/body/progression, tier-1 model, MQ and DWT unit
tests, a JP2/J2K profile matrix against committed reference pixels, WebP
animation tests, a TIFF/compression migration test, and a deterministic fuzz
smoke. An optional GCC-only JP2 coverage fuzzer uses
`-DQW_JP2_BUILD_COVERAGE_FUZZER=ON`.

## Documentation

See [docs/usage.md](docs/usage.md) for recipes, scan-cleanup pipelines and limit notes.

## License

MIT. See [LICENSE](LICENSE).
