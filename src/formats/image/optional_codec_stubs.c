#include "image_internal.h"
#include "quirewise/status.h"
#include <string.h>

/* Only entries without a real implementation live here. AVIF/HEIC/RAW
 * structure probes are provided by heif_parser.c and raw_parser.c; the JPEG XL
 * parser source is not part of the default target, so its probe stays stubbed. */

static int unsupported_page(QwPageImage *out_page)
{
    if (out_page) memset(out_page, 0, sizeof(*out_page));
    return QW_STATUS_ERR_UNSUPPORTED;
}

int qw_jxl_probe(const void *data, size_t size) { (void)data; (void)size; return 0; }

int qw_avif_decode_page(const void *data, size_t size, QwPageImage *out_page) { (void)data; (void)size; return unsupported_page(out_page); }
int qw_avif_encode_page(const QwPageImage *page, uint8_t **out_data, size_t *out_size) { (void)page; if (out_data) *out_data = NULL; if (out_size) *out_size = 0; return QW_STATUS_ERR_UNSUPPORTED; }
int qw_heic_decode_page(const void *data, size_t size, QwPageImage *out_page) { (void)data; (void)size; return unsupported_page(out_page); }
int qw_heic_encode_page(const QwPageImage *page, uint8_t **out_data, size_t *out_size) { (void)page; if (out_data) *out_data = NULL; if (out_size) *out_size = 0; return QW_STATUS_ERR_UNSUPPORTED; }
int qw_jxl_decode_page(const void *data, size_t size, QwPageImage *out_page) { (void)data; (void)size; return unsupported_page(out_page); }
int qw_jxl_encode_page(const QwPageImage *page, uint8_t **out_data, size_t *out_size) { (void)page; if (out_data) *out_data = NULL; if (out_size) *out_size = 0; return QW_STATUS_ERR_UNSUPPORTED; }
int qw_raw_decode_page(const void *data, size_t size, QwPageImage *out_page) { (void)data; (void)size; return unsupported_page(out_page); }
int qw_pcd_probe(const void *data, size_t size) { (void)data; (void)size; return 0; }
int qw_pcd_decode_page(const void *data, size_t size, QwPageImage *out_page) { (void)data; (void)size; return unsupported_page(out_page); }
