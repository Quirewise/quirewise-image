#ifndef QUIREWISE_IMAGE_FOLDER_H
#define QUIREWISE_IMAGE_FOLDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QUIREWISE_IMAGE_FOLDER_ABI_VERSION 1u

#define QW_IMAGE_FOLDER_DEFAULT_MAX_ENTRIES 10000u
#define QW_IMAGE_FOLDER_DEFAULT_MAX_TOTAL_BYTES UINT64_C(1073741824)
#define QW_IMAGE_FOLDER_DEFAULT_MAX_FILE_BYTES UINT64_C(268435456)

typedef enum QwImageFolderStatus {
    QW_IMAGE_FOLDER_OK = 0,
    QW_IMAGE_FOLDER_INVALID_ARGUMENT = -1,
    QW_IMAGE_FOLDER_OUT_OF_MEMORY = -2,
    QW_IMAGE_FOLDER_IO_ERROR = -3,
    QW_IMAGE_FOLDER_EMPTY = -4,
    QW_IMAGE_FOLDER_RESOURCE_LIMIT = -5
} QwImageFolderStatus;

typedef struct QwImageFileEntry {
    /* UTF-8 full path owned by QwImageFolder and released with its free function. */
    char *path;
    uint64_t file_size;
} QwImageFileEntry;

typedef struct QwImageFolderOptions {
    /* A zero limit selects the documented default. */
    size_t max_entries;
    uint64_t max_total_bytes;
    uint64_t max_single_file_bytes;
} QwImageFolderOptions;

typedef struct QwImageFolder {
    QwImageFileEntry *entries;
    size_t count;
    uint64_t total_bytes;
} QwImageFolder;

uint32_t qw_image_folder_abi_version(void);
int qw_image_folder_is_directory(const char *path);

/* Pass a zero-initialized output value. Lists supported raster-image names and Lists supported raster-image names in natural filename order. The directory is not traversed
 * recursively. Windows paths are UTF-8; POSIX paths retain native path bytes. */
QwImageFolderStatus qw_image_folder_open(const char *path,
                                         const QwImageFolderOptions *options,
                                         QwImageFolder *out_folder);
void qw_image_folder_free(QwImageFolder *folder);
const char *qw_image_folder_status_string(QwImageFolderStatus status);

#ifdef __cplusplus
}
#endif

#endif
