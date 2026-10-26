#include "quirewise/image.h"
#include "../../core/byte_utils.h"

#include <string.h>

int qw_image_name_is_supported(const char *name)
{
    const char *base;
    const char *cursor;
    const char *extension;
    static const char *const supported[] = {
        "jpg", "jpeg", "png", "bmp", "gif", "tif", "tiff", "webp",
        "tga", "psd", "hdr", "pic", "pnm", "ppm", "pgm", "pbm", "pam",
        "qoi", "wbmp", "xbm", "ico", "cur",
        "jp2", "j2k", "j2c", "jpf", "jpx", "pcx", "pfm", "icns", "dds", "xpm",
        "sgi", "rgb", "rgba", "bw", "ras", "sun", "dcm", "dicom", "psb", "miff", "mng", "apng", "exr",
        "xcf", "pcd"
    };
    if (!name) return 0;
    base = name;
    for (cursor = name; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1;
    }
    extension = strrchr(base, '.');
    if (!extension || !extension[1]) return 0;
    ++extension;
    for (size_t i = 0u; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        const char *candidate = supported[i];
        const char *left = extension;
        while (*left && *candidate &&
               qw_ascii_lower((unsigned char)*left) ==
               qw_ascii_lower((unsigned char)*candidate)) {
            ++left;
            ++candidate;
        }
        if (!*left && !*candidate) return 1;
    }
    return 0;
}
