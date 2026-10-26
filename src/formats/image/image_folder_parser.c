#include "quirewise/image_folder.h"
#include "image_name.h"
#include "natural_sort.h"
#include "platform_unicode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

static int qw_image_folder_name_supported(const char *name)
{
    return qw_image_name_is_supported(name);
}

static char *qw_image_folder_join_path(const char *directory, const char *name)
{
    size_t directory_size;
    size_t name_size;
    int add_separator;
    size_t total;
    char *joined;

    if (!directory || !name) return NULL;
    directory_size = strlen(directory);
    name_size = strlen(name);
#if defined(_WIN32)
    add_separator = directory_size != 0u &&
                    directory[directory_size - 1u] != '/' &&
                    directory[directory_size - 1u] != '\\';
#else
    add_separator = directory_size != 0u && directory[directory_size - 1u] != '/';
#endif
    if (directory_size > SIZE_MAX - name_size - (size_t)add_separator - 1u) {
        return NULL;
    }
    total = directory_size + (size_t)add_separator + name_size + 1u;
    joined = (char *)malloc(total);
    if (!joined) return NULL;
    memcpy(joined, directory, directory_size);
    if (add_separator) {
#if defined(_WIN32)
        joined[directory_size++] = '\\';
#else
        joined[directory_size++] = '/';
#endif
    }
    memcpy(joined + directory_size, name, name_size + 1u);
    return joined;
}

static int qw_image_folder_compare_entries(const void *left, const void *right)
{
    const QwImageFileEntry *a = (const QwImageFileEntry *)left;
    const QwImageFileEntry *b = (const QwImageFileEntry *)right;
    return qw_natural_name_compare(a->path, b->path);
}

static void qw_image_folder_limits(const QwImageFolderOptions *options,
                                   QwImageFolderOptions *limits)
{
    limits->max_entries = options && options->max_entries
        ? options->max_entries : QW_IMAGE_FOLDER_DEFAULT_MAX_ENTRIES;
    limits->max_total_bytes = options && options->max_total_bytes
        ? options->max_total_bytes : QW_IMAGE_FOLDER_DEFAULT_MAX_TOTAL_BYTES;
    limits->max_single_file_bytes = options && options->max_single_file_bytes
        ? options->max_single_file_bytes : QW_IMAGE_FOLDER_DEFAULT_MAX_FILE_BYTES;
}

static QwImageFolderStatus qw_image_folder_add(QwImageFolder *folder,
                                                size_t *capacity,
                                                const QwImageFolderOptions *limits,
                                                char *path,
                                                uint64_t file_size)
{
    QwImageFileEntry *entries;
    size_t next_capacity;

    if (!folder || !capacity || !limits || !path) {
        free(path);
        return QW_IMAGE_FOLDER_INVALID_ARGUMENT;
    }
    if (folder->count >= limits->max_entries ||
        file_size > limits->max_single_file_bytes ||
        file_size > limits->max_total_bytes ||
        folder->total_bytes > limits->max_total_bytes - file_size) {
        free(path);
        return QW_IMAGE_FOLDER_RESOURCE_LIMIT;
    }
    if (folder->count == *capacity) {
        next_capacity = *capacity ? *capacity * 2u : 16u;
        if (next_capacity < *capacity ||
            next_capacity > SIZE_MAX / sizeof(*entries)) {
            free(path);
            return QW_IMAGE_FOLDER_OUT_OF_MEMORY;
        }
        entries = (QwImageFileEntry *)realloc(
            folder->entries, next_capacity * sizeof(*entries));
        if (!entries) {
            free(path);
            return QW_IMAGE_FOLDER_OUT_OF_MEMORY;
        }
        folder->entries = entries;
        *capacity = next_capacity;
    }
    folder->entries[folder->count].path = path;
    folder->entries[folder->count].file_size = file_size;
    ++folder->count;
    folder->total_bytes += file_size;
    return QW_IMAGE_FOLDER_OK;
}

uint32_t qw_image_folder_abi_version(void)
{
    return QUIREWISE_IMAGE_FOLDER_ABI_VERSION;
}

int qw_image_folder_is_directory(const char *path)
{
    if (!path || !*path) return 0;
#if defined(_WIN32)
    {
        wchar_t *wide_path = qw_platform_utf8_to_wide(path);
        DWORD attributes;
        if (!wide_path) return 0;
        attributes = GetFileAttributesW(wide_path);
        free(wide_path);
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
#else
    {
        struct stat status;
        return stat(path, &status) == 0 && S_ISDIR(status.st_mode);
    }
#endif
}

#if defined(_WIN32)
static QwImageFolderStatus qw_image_folder_enumerate(
    const char *path, const QwImageFolderOptions *limits,
    QwImageFolder *folder)
{
    wchar_t *wide_directory = qw_platform_utf8_to_wide(path);
    wchar_t *pattern;
    size_t directory_size;
    size_t pattern_size;
    WIN32_FIND_DATAW item;
    HANDLE search;
    size_t capacity = 0u;
    DWORD last_error;
    QwImageFolderStatus status = QW_IMAGE_FOLDER_OK;

    if (!wide_directory) return QW_IMAGE_FOLDER_INVALID_ARGUMENT;
    directory_size = wcslen(wide_directory);
    if (directory_size > (SIZE_MAX / sizeof(*pattern)) - 3u) {
        free(wide_directory);
        return QW_IMAGE_FOLDER_INVALID_ARGUMENT;
    }
    pattern_size = directory_size + 3u;
    pattern = (wchar_t *)malloc(pattern_size * sizeof(*pattern));
    if (!pattern) {
        free(wide_directory);
        return QW_IMAGE_FOLDER_OUT_OF_MEMORY;
    }
    memcpy(pattern, wide_directory, directory_size * sizeof(*pattern));
    if (directory_size && pattern[directory_size - 1u] != L'\\' &&
        pattern[directory_size - 1u] != L'/') {
        pattern[directory_size++] = L'\\';
    }
    pattern[directory_size++] = L'*';
    pattern[directory_size] = L'\0';
    free(wide_directory);

    search = FindFirstFileW(pattern, &item);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) return QW_IMAGE_FOLDER_IO_ERROR;
    do {
        char *name;
        char *full_path;
        uint64_t file_size;
        if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        name = qw_platform_wide_to_utf8(item.cFileName);
        if (!name) continue;
        if (!qw_image_folder_name_supported(name)) {
            free(name);
            continue;
        }
        full_path = qw_image_folder_join_path(path, name);
        free(name);
        if (!full_path) {
            status = QW_IMAGE_FOLDER_OUT_OF_MEMORY;
            break;
        }
        file_size = ((uint64_t)item.nFileSizeHigh << 32) |
                    (uint64_t)item.nFileSizeLow;
        status = qw_image_folder_add(folder, &capacity, limits,
                                     full_path, file_size);
        if (status != QW_IMAGE_FOLDER_OK) break;
    } while (FindNextFileW(search, &item));
    last_error = GetLastError();
    FindClose(search);
    if (status == QW_IMAGE_FOLDER_OK && last_error != ERROR_NO_MORE_FILES) {
        status = QW_IMAGE_FOLDER_IO_ERROR;
    }
    return status;
}
#else
static QwImageFolderStatus qw_image_folder_enumerate(
    const char *path, const QwImageFolderOptions *limits,
    QwImageFolder *folder)
{
    DIR *directory = opendir(path);
    struct dirent *item;
    size_t capacity = 0u;
    QwImageFolderStatus status = QW_IMAGE_FOLDER_OK;
    if (!directory) return QW_IMAGE_FOLDER_IO_ERROR;
    while ((item = readdir(directory)) != NULL) {
        char *full_path;
        struct stat file_status;
        if (!qw_image_folder_name_supported(item->d_name)) continue;
        full_path = qw_image_folder_join_path(path, item->d_name);
        if (!full_path) {
            status = QW_IMAGE_FOLDER_OUT_OF_MEMORY;
            break;
        }
        if (stat(full_path, &file_status) != 0 || !S_ISREG(file_status.st_mode)) {
            free(full_path);
            continue;
        }
        if (file_status.st_size < 0) {
            free(full_path);
            status = QW_IMAGE_FOLDER_IO_ERROR;
            break;
        }
        status = qw_image_folder_add(folder, &capacity, limits, full_path,
                                     (uint64_t)file_status.st_size);
        if (status != QW_IMAGE_FOLDER_OK) break;
    }
    closedir(directory);
    return status;
}
#endif

void qw_image_folder_free(QwImageFolder *folder)
{
    if (!folder) return;
    for (size_t i = 0u; i < folder->count; ++i) {
        free(folder->entries[i].path);
    }
    free(folder->entries);
    memset(folder, 0, sizeof(*folder));
}

QwImageFolderStatus qw_image_folder_open(const char *path,
                                         const QwImageFolderOptions *options,
                                         QwImageFolder *out_folder)
{
    QwImageFolderOptions limits;
    QwImageFolderStatus status;
    if (!out_folder) return QW_IMAGE_FOLDER_INVALID_ARGUMENT;
    memset(out_folder, 0, sizeof(*out_folder));
    if (!path || !*path) return QW_IMAGE_FOLDER_INVALID_ARGUMENT;
    if (!qw_image_folder_is_directory(path)) return QW_IMAGE_FOLDER_IO_ERROR;
    qw_image_folder_limits(options, &limits);
    status = qw_image_folder_enumerate(path, &limits, out_folder);
    if (status != QW_IMAGE_FOLDER_OK) {
        qw_image_folder_free(out_folder);
        return status;
    }
    if (!out_folder->count) return QW_IMAGE_FOLDER_EMPTY;
    qsort(out_folder->entries, out_folder->count,
          sizeof(*out_folder->entries), qw_image_folder_compare_entries);
    return QW_IMAGE_FOLDER_OK;
}

const char *qw_image_folder_status_string(QwImageFolderStatus status)
{
    switch (status) {
    case QW_IMAGE_FOLDER_OK: return "ok";
    case QW_IMAGE_FOLDER_INVALID_ARGUMENT: return "invalid argument";
    case QW_IMAGE_FOLDER_OUT_OF_MEMORY: return "out of memory";
    case QW_IMAGE_FOLDER_IO_ERROR: return "directory access failed";
    case QW_IMAGE_FOLDER_EMPTY: return "no supported image entries";
    case QW_IMAGE_FOLDER_RESOURCE_LIMIT: return "image folder resource limit exceeded";
    default: return "unknown image folder error";
    }
}
