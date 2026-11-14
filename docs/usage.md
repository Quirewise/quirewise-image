# quirewise-image usage guide

This guide covers the task-level workflows of `Quirewise::Image`. The public API
lives in `quirewise/image.h`, `quirewise/image_folder.h` and
`quirewise/image_processing.h`.

## 1. Build and integrate

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Consumer CMake:

```cmake
find_package(QuirewiseImage CONFIG REQUIRED)
target_link_libraries(your_app PRIVATE Quirewise::Image)
```

`Quirewise::Image` bundles the decode, folder and processing targets. There is
nothing to initialize at runtime: every entry point is a stateless function.

## 2. Core model

Two pixel types cover the API:

- `QwImage`: a decoded still image with `pixels`, `size`, `stride`, `width`,
  `height` and `format` (`QW_IMAGE_PIXEL_GRAY8` or `QW_IMAGE_PIXEL_RGB24`).
- `QwGrayImage`: the working type for scan cleanup, tightly packed Gray8
  pixels plus `width` and `height`. There is no stride; `size` is
  `width * height`.

Ownership rules:

- `qw_image_decode` allocates `out_image->pixels`; release with `qw_image_free`.
- `qw_gray_image_copy`/`adopt` require an empty, zero-initialized output value
  and are released with `qw_gray_image_release`; `adopt` takes over the buffer.
- `qw_gray_image_split_copy` also requires an empty output value.
- Input buffers are borrowed; no call keeps a pointer after returning.
- Processing replaces pixels and may change dimensions. After a failure the
  value is still releasable, but its contents are unspecified.

Status conventions:

| API family | Success | Failure |
|---|---|---|
| Decode | `QW_IMAGE_OK` (0) | negative `QwImageStatus` |
| Folder | `QW_IMAGE_FOLDER_OK` (0) | negative `QwImageFolderStatus` |
| Processing | `QW_IMAGE_PROCESSING_OK` (0) | negative `QwImageProcessingStatus` |
| Probe / filename check | `1` | `0` |

## 3. Probe, decode and inspect

Read the whole file first; the decoder never performs file I/O. This helper is
reused below:

```c
#include <quirewise/image.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned char *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    unsigned char *data;
    long length;

    if (!file || fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file) fclose(file);
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
```

Gate on the filename and signature, then decode:

```c
QwImage image = {0};
size_t size = 0;
unsigned char *data;
int status;

if (!qw_image_name_is_supported("scan.png")) return 1; /* extension gate */
data = read_file("scan.png", &size);
if (!data) return 1;
if (!qw_image_probe(data, size)) {          /* 0: no supported signature */
    free(data);
    return 1;
}
status = qw_image_decode(data, size, &image);
free(data);                                  /* pixels are independent */
if (status != QW_IMAGE_OK) {
    fprintf(stderr, "decode: %s\n", qw_image_status_string(status));
    return 1;
}
printf("%ux%u stride=%zu bytes=%zu\n", image.width, image.height,
       image.stride, image.size);
qw_image_free(&image);
```

A probe result of `1` only means the signature was recognised, not that the
payload decodes.

## 4. Enumerate an image folder

`qw_image_folder_open` is non-recursive, filters by the same extensions as
`qw_image_name_is_supported`, and sorts naturally (`page2.png` before
`page10.png`).

```c
QwImageFolder folder = {0};
QwImageFolderOptions options = {0};
size_t index;
int status;
options.max_entries = 500;                     /* 0 -> default 10000 */
options.max_total_bytes = 64u * 1024u * 1024u; /* 0 -> default 1 GiB */
/* max_single_file_bytes: 0 -> default 256 MiB */
status = qw_image_folder_open("F:/scans/chapter-01", &options, &folder);
if (status != QW_IMAGE_FOLDER_OK) {
    fprintf(stderr, "folder: %s\n", qw_image_folder_status_string(status));
    return 1;
}
for (index = 0; index < folder.count; ++index) {
    printf("%s (%llu bytes)\n", folder.entries[index].path,
           (unsigned long long)folder.entries[index].file_size);
}
qw_image_folder_free(&folder);               /* frees every path */
```

`QW_IMAGE_FOLDER_EMPTY` means the directory exists but holds no supported image
names; the output value is still safe to free. Pass `options == NULL` to accept
all defaults. Each `entries[i].path` is already a joined full path, ready for
`read_file` and `qw_image_decode`.

## 5. Clean up a scanned page

Processing functions mutate a `QwGrayImage` in place. A common order is
illumination, then geometry, then crop. Stop at the first failure if the
intermediate result is not useful.

```c
QwGrayImage gray = {0};
QwImageProcessingStatus st;

/* pixels/size come from a grayscale source, e.g. converted from QwImage */
st = qw_gray_image_copy(pixels, size, width, height, &gray);
if (st != QW_IMAGE_PROCESSING_OK) return 1;
st = qw_gray_image_normalize_illumination(&gray);
if (st == QW_IMAGE_PROCESSING_OK) st = qw_gray_image_deskew(&gray);
if (st == QW_IMAGE_PROCESSING_OK)
    st = qw_gray_image_correct_perspective(&gray);
if (st == QW_IMAGE_PROCESSING_OK) st = qw_gray_image_crop(&gray, 8u);
if (st != QW_IMAGE_PROCESSING_OK) {
    fprintf(stderr, "cleanup: %s\n", qw_image_processing_status_string(st));
    qw_gray_image_release(&gray);
    return 1;
}
/* gray.pixels, gray.width and gray.height describe the cleaned page */
qw_gray_image_release(&gray);
```

- `qw_gray_image_dewarp` is an alternative to perspective correction for
  curved book pages; running both is usually counterproductive.
- `qw_gray_image_crop` trims to the detected content, then expands by `padding`
  pixels per side, clamped to the image bounds.

## 6. Split a two-page spread

`qw_gray_image_split_copy` finds the gutter and copies one half without
modifying the source. The destination must be empty.

```c
QwGrayImage left = {0};
QwImageProcessingStatus st;

st = qw_gray_image_split_copy(&spread, QW_GRAY_IMAGE_SPLIT_LEFT, &left);
if (st != QW_IMAGE_PROCESSING_OK) {
    qw_gray_image_release(&left);
    return 1;
}
/* the right half uses the same call with QW_GRAY_IMAGE_SPLIT_RIGHT */
qw_gray_image_release(&left);
```

`QW_IMAGE_PROCESSING_ERR_UNSUPPORTED` usually means no gutter was found, for
example on a single page or a blank scan.

## 7. Error handling by status

`QwImageStatus` values used by this package:

| Value | Meaning | Reaction |
|---|---|---|
| `QW_IMAGE_ERR_INVALID_ARGUMENT` | null pointer, zero size, bad output slot | fix the call |
| `QW_IMAGE_ERR_OUT_OF_MEMORY` | allocation failed | abort or retry later |
| `QW_IMAGE_ERR_FORMAT` | signature matched, structure invalid | reject the file |
| `QW_IMAGE_ERR_UNSUPPORTED` | valid container, unsupported variant | reject or use another tool |
| `QW_IMAGE_ERR_DECODE` | entropy or pixel decoding failed | reject the file |
| `QW_IMAGE_ERR_RESOURCE_LIMIT` | dimension or pixel cap exceeded | downscale or reject |

`QW_IMAGE_ERR_IO` and `QW_IMAGE_ERR_EXTERNAL_DECODER_REQUIRED` belong to the
shared status vocabulary but are not produced here. Folder and processing APIs
use their own enums; print diagnostics with the matching `*_status_string`.

## 8. Memory, lifetime and threading

- No pointer to caller memory survives a call.
- Every allocation has a release function. Do not `free` pixel pointers
  directly; the release functions also reset the structs.
- `qw_gray_image_adopt` transfers ownership only on success.
- There is no global mutable state, so independent calls on different buffers
  may run concurrently. Never process one image value from two threads.

## 9. Limits and format notes

- Maximum dimension 32768 pixels and 20,000,000 decoded pixels, both checked
  before allocation.
- Output is always Gray8 or RGB24; alpha is composited on white, palettes are
  expanded and 16-bit sources are reduced to 8 bits.
- One image per call: the first frame, default image, first top-level IFD or
  first entry. Animated WebP returns `QW_IMAGE_ERR_UNSUPPORTED`.
- EXIF orientation and ICC profiles are not applied or preserved.
- Folder defaults: 10,000 entries, 1 GiB total bytes, 256 MiB per file. A limit
  breach returns `QW_IMAGE_FOLDER_RESOURCE_LIMIT` after freeing the partial
  listing.
- Camera RAW, AVIF, HEIC, JPEG XL and PCD have no decoder. DNG/TIFF-CFA probes
  may succeed, after which decode returns `QW_IMAGE_ERR_UNSUPPORTED`. `.icns`
  is a recognised extension without a decoder, so a listing can contain
  entries that decode rejects.
- There is no public encoder; PNG output elsewhere in Quirewise comes from other
  packages.

## 10. Related packages

- `Quirewise::Page` handles DjVu/UVZ/CAJ/CEB page containers; extracted page
  bytes that are ordinary images can be decoded here.
- `Quirewise::PDG` uses this package to rasterise embedded image payloads.
- `Quirewise::Document` and `Quirewise::Archive` use it for embedded covers.
