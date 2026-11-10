/* First-party JPEG decoder.
 *
 * Supports baseline (SOF0) and progressive (SOF2) Huffman-coded images with
 * 8-bit samples, 1..3 components (grayscale, YCbCr), 1x/2x chroma sampling,
 * restart intervals and both sequential and progressive scans. The IDCT is a
 * straightforward separable double-precision transform; chroma upsampling
 * uses the standard 3:1 triangle filter for 2x sampling. */

#include "jpeg_decoder.h"

#include <stdlib.h>
#include <string.h>

#define QW_JPEG_MAX_COMPONENTS 4
#define QW_JPEG_MAX_PIXELS ((size_t)100u * 1000u * 1000u)

static const uint8_t jpeg_zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* 8-point IDCT basis: cos((2x+1)u*pi/16), with the u==0 scaling. */
static const double jpeg_idct_basis[8][8] = {
    { 0.70710678118654757, 0.98078528040323043, 0.92387953251128674,
      0.83146961230254524, 0.70710678118654757, 0.55557023301960229,
      0.38268343236508978, 0.19509032201612828 },
    { 0.70710678118654757, 0.83146961230254524, 0.38268343236508978,
      -0.19509032201612828, -0.70710678118654757, -0.98078528040323043,
      -0.92387953251128674, -0.55557023301960229 },
    { 0.70710678118654757, 0.55557023301960229, -0.38268343236508978,
      -0.98078528040323043, -0.70710678118654757, 0.19509032201612828,
      0.92387953251128674, 0.83146961230254524 },
    { 0.70710678118654757, 0.19509032201612828, -0.92387953251128674,
      -0.55557023301960229, 0.70710678118654757, 0.83146961230254524,
      -0.38268343236508978, -0.98078528040323043 },
    { 0.70710678118654757, -0.19509032201612828, -0.92387953251128674,
      0.55557023301960229, 0.70710678118654757, -0.83146961230254524,
      -0.38268343236508978, 0.98078528040323043 },
    { 0.70710678118654757, -0.55557023301960229, -0.38268343236508978,
      0.98078528040323043, -0.70710678118654757, -0.19509032201612828,
      0.92387953251128674, -0.83146961230254524 },
    { 0.70710678118654757, -0.83146961230254524, 0.38268343236508978,
      0.19509032201612828, -0.70710678118654757, 0.98078528040323043,
      -0.92387953251128674, 0.55557023301960229 },
    { 0.70710678118654757, -0.98078528040323043, 0.92387953251128674,
      -0.83146961230254524, 0.70710678118654757, -0.55557023301960229,
      0.38268343236508978, -0.19509032201612828 }
};

typedef struct QwJpegHuffman {
    uint8_t bits[17];
    uint8_t values[256];
    int32_t mincode[17];
    int32_t maxcode[18];
    int32_t valptr[17];
    int valid;
} QwJpegHuffman;

typedef struct QwJpegComponent {
    int id;
    int h;
    int v;
    int tq;
    int td;
    int ta;
    int blocks_x; /* coefficient plane blocks (MCU based) */
    int blocks_y;
    int comp_blocks_x; /* actual blocks for non-interleaved scans */
    int comp_blocks_y;
    int plane_w;
    int plane_h;
    int16_t *coeff;
    uint8_t *plane;
} QwJpegComponent;

typedef struct QwJpegDecoder {
    const uint8_t *data;
    size_t size;
    size_t pos;
    int width;
    int height;
    int ncomp;
    int max_h;
    int max_v;
    int mcu_x;
    int mcu_y;
    int progressive;
    int restart_interval;
    int restart_pending;
    QwJpegComponent comp[QW_JPEG_MAX_COMPONENTS];
    uint16_t qtable[4][64];
    int qtable_valid[4];
    QwJpegHuffman dc[4];
    QwJpegHuffman ac[4];
    uint32_t bitbuf;
    int bitcnt;
    int hit_marker;
    int marker;
    int eof;
    int saw_jfif;
    int saw_adobe;
    int adobe_transform;
    int eobrun;
} QwJpegDecoder;

static int jpeg_byte(QwJpegDecoder *d)
{
    if (d->pos >= d->size) {
        d->eof = 1;
        return -1;
    }
    return d->data[d->pos++];
}

static int jpeg_u16(QwJpegDecoder *d)
{
    int hi = jpeg_byte(d);
    int lo = jpeg_byte(d);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 8) | lo;
}

/* Read one entropy-coded byte, handling 0xFF00 stuffing. A real marker
 * rewinds the cursor to the marker byte and sets hit_marker so the outer
 * marker loop can process it normally. */
static int jpeg_entropy_byte(QwJpegDecoder *d)
{
    int b;
    size_t ffpos;
    if (d->hit_marker) return -1;
    if (d->pos >= d->size) {
        d->eof = 1;
        return -1;
    }
    b = d->data[d->pos++];
    if (b != 0xff) return b;
    ffpos = d->pos - 1;
    for (;;) {
        int c;
        if (d->pos >= d->size) {
            d->eof = 1;
            return -1;
        }
        c = d->data[d->pos];
        if (c == 0xff) {
            ffpos = d->pos;
            ++d->pos;
            continue;
        }
        if (c == 0x00) {
            ++d->pos;
            return 0xff;
        }
        d->pos = ffpos;
        d->marker = c;
        d->hit_marker = 1;
        return -1;
    }
}

static int jpeg_get_bit(QwJpegDecoder *d)
{
    int bit;
    if (d->bitcnt == 0) {
        int b = jpeg_entropy_byte(d);
        if (b < 0) return 0;
        d->bitbuf = (uint32_t)b;
        d->bitcnt = 8;
    }
    --d->bitcnt;
    bit = (int)((d->bitbuf >> d->bitcnt) & 1u);
    return bit;
}

static int jpeg_get_bits(QwJpegDecoder *d, int count)
{
    int value = 0;
    int i;
    for (i = 0; i < count; ++i) value = (value << 1) | jpeg_get_bit(d);
    return value;
}

static int jpeg_huff_decode(QwJpegDecoder *d, const QwJpegHuffman *h)
{
    int32_t code = 0;
    int length;
    for (length = 1; length <= 16; ++length) {
        code = (code << 1) | jpeg_get_bit(d);
        if (h->maxcode[length] >= 0 && code <= h->maxcode[length])
            return h->values[h->valptr[length] + (int)(code - h->mincode[length])];
    }
    d->eof = 1;
    return 0;
}

static int jpeg_extend(int value, int bits)
{
    if (bits == 0) return 0;
    if (value < (1 << (bits - 1))) value -= (1 << bits) - 1;
    return value;
}

static void jpeg_huffman_build(QwJpegHuffman *h, const uint8_t *counts,
                               const uint8_t *values, int value_count)
{
    int code = 0;
    int k = 0;
    int i;
    memset(h, 0, sizeof(*h));
    memcpy(h->bits, counts, 17);
    memcpy(h->values, values, (size_t)value_count);
    for (i = 1; i <= 16; ++i) {
        h->valptr[i] = k;
        h->mincode[i] = code;
        code += counts[i];
        k += counts[i];
        h->maxcode[i] = counts[i] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    h->valid = 1;
}

/* ------------------------------------------------------------------ */
/* IDCT                                                                */
/* ------------------------------------------------------------------ */

static void jpeg_idct_block(const int16_t *coeff, const uint16_t *qt,
                            uint8_t *out, int stride)
{
    double block[64];
    double tmp[64];
    int u;
    int v;
    int x;
    int y;

    for (v = 0; v < 8; ++v) {
        for (u = 0; u < 8; ++u)
            block[v * 8 + u] = (double)coeff[v * 8 + u] * (double)qt[v * 8 + u];
    }
    /* Horizontal pass. */
    for (v = 0; v < 8; ++v) {
        for (x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (u = 0; u < 8; ++u)
                sum += block[v * 8 + u] * jpeg_idct_basis[x][u];
            tmp[v * 8 + x] = sum * 0.5;
        }
    }
    /* Vertical pass. */
    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            double sum = 0.0;
            for (v = 0; v < 8; ++v)
                sum += tmp[v * 8 + x] * jpeg_idct_basis[y][v];
            {
                int value = (int)(sum * 0.5 + 128.5);
                if (value < 0) value = 0;
                else if (value > 255) value = 255;
                out[y * stride + x] = (uint8_t)value;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Marker parsing                                                      */
/* ------------------------------------------------------------------ */

static const uint8_t *jpeg_segment(QwJpegDecoder *d, size_t *out_size)
{
    int length = jpeg_u16(d);
    const uint8_t *segment;
    if (length < 2) {
        d->eof = 1;
        return NULL;
    }
    length -= 2;
    if (d->pos + (size_t)length > d->size) {
        d->eof = 1;
        return NULL;
    }
    segment = d->data + d->pos;
    d->pos += (size_t)length;
    *out_size = (size_t)length;
    return segment;
}

static int jpeg_parse_dqt(QwJpegDecoder *d, const uint8_t *segment, size_t size)
{
    size_t pos = 0;
    while (pos < size) {
        uint8_t spec = segment[pos++];
        int precision = spec >> 4;
        int table = spec & 15;
        int i;
        if (table > 3 || precision > 1) return -1;
        if ((size_t)(precision ? 128 : 64) > size - pos) return -1;
        for (i = 0; i < 64; ++i) {
            int value;
            if (precision) {
                value = (segment[pos] << 8) | segment[pos + 1];
                pos += 2;
            } else {
                value = segment[pos++];
            }
            d->qtable[table][jpeg_zigzag[i]] = (uint16_t)value;
        }
        d->qtable_valid[table] = 1;
    }
    return 0;
}

static int jpeg_parse_dht(QwJpegDecoder *d, const uint8_t *segment, size_t size)
{
    size_t pos = 0;
    while (pos < size) {
        uint8_t spec;
        int table_class;
        int table;
        uint8_t counts[17];
        uint8_t values[256];
        int total = 0;
        int i;
        if (size - pos < 17) return -1;
        spec = segment[pos++];
        table_class = spec >> 4;
        table = spec & 15;
        if (table_class > 1 || table > 3) return -1;
        counts[0] = 0;
        for (i = 1; i <= 16; ++i) {
            counts[i] = segment[pos++];
            total += counts[i];
        }
        if (total > 256 || (size_t)total > size - pos) return -1;
        memcpy(values, segment + pos, (size_t)total);
        pos += (size_t)total;
        if (table_class == 0)
            jpeg_huffman_build(&d->dc[table], counts, values, total);
        else
            jpeg_huffman_build(&d->ac[table], counts, values, total);
    }
    return 0;
}

static int jpeg_parse_sof(QwJpegDecoder *d, int marker, const uint8_t *segment,
                          size_t size)
{
    int precision;
    int n;
    int i;
    if (marker != 0xc0 && marker != 0xc2) return -1;
    if (size < 6) return -1;
    precision = segment[0];
    if (precision != 8) return -1;
    d->height = (segment[1] << 8) | segment[2];
    d->width = (segment[3] << 8) | segment[4];
    n = segment[5];
    if (n < 1 || n > 3) return -1;
    if ((size_t)(6 + n * 3) != size) return -1;
    if (d->width <= 0 || d->height <= 0) return -1;
    if ((size_t)d->width * (size_t)d->height > QW_JPEG_MAX_PIXELS) return -1;
    d->ncomp = n;
    d->progressive = marker == 0xc2;
    d->max_h = 1;
    d->max_v = 1;
    for (i = 0; i < n; ++i) {
        const uint8_t *c = segment + 6 + i * 3;
        QwJpegComponent *comp = &d->comp[i];
        comp->id = c[0];
        comp->h = c[1] >> 4;
        comp->v = c[1] & 15;
        comp->tq = c[2];
        comp->td = 0;
        comp->ta = 0;
        if (comp->h < 1 || comp->h > 4 || comp->v < 1 || comp->v > 4) return -1;
        if (comp->tq > 3) return -1;
        if (comp->h > d->max_h) d->max_h = comp->h;
        if (comp->v > d->max_v) d->max_v = comp->v;
        {
            int j;
            for (j = 0; j < i; ++j)
                if (d->comp[j].id == comp->id) return -1;
        }
    }
    d->mcu_x = (d->width + 8 * d->max_h - 1) / (8 * d->max_h);
    d->mcu_y = (d->height + 8 * d->max_v - 1) / (8 * d->max_v);
    for (i = 0; i < n; ++i) {
        QwJpegComponent *comp = &d->comp[i];
        int comp_w = (d->width * comp->h + d->max_h - 1) / d->max_h;
        int comp_h = (d->height * comp->v + d->max_v - 1) / d->max_v;
        comp->blocks_x = d->mcu_x * comp->h;
        comp->blocks_y = d->mcu_y * comp->v;
        comp->comp_blocks_x = (comp_w + 7) / 8;
        comp->comp_blocks_y = (comp_h + 7) / 8;
        comp->plane_w = comp->blocks_x * 8;
        comp->plane_h = comp->blocks_y * 8;
        comp->coeff = (int16_t *)calloc((size_t)comp->blocks_x *
                                            (size_t)comp->blocks_y * 64u,
                                        sizeof(int16_t));
        comp->plane = (uint8_t *)calloc((size_t)comp->plane_w *
                                            (size_t)comp->plane_h,
                                        1u);
        if (!comp->coeff || !comp->plane) return -1;
    }
    return 0;
}

static void jpeg_decode_block_baseline(QwJpegDecoder *d, QwJpegComponent *comp,
                                       int16_t *block, int *pred)
{
    int t = jpeg_huff_decode(d, &d->dc[comp->td]);
    int diff = jpeg_extend(t ? jpeg_get_bits(d, t) : 0, t);
    int k;
    *pred += diff;
    block[0] = (int16_t)*pred;
    k = 1;
    while (k < 64) {
        int rs = jpeg_huff_decode(d, &d->ac[comp->ta]);
        int r = rs >> 4;
        int s = rs & 15;
        if (s == 0) {
            if (r != 15) break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) break;
        block[jpeg_zigzag[k]] = (int16_t)jpeg_extend(jpeg_get_bits(d, s), s);
        ++k;
    }
}

static void jpeg_decode_block_progressive_dc_first(QwJpegDecoder *d,
                                                   QwJpegComponent *comp,
                                                   int16_t *block, int *pred,
                                                   int al)
{
    int t = jpeg_huff_decode(d, &d->dc[comp->td]);
    int diff = jpeg_extend(t ? jpeg_get_bits(d, t) : 0, t);
    *pred += diff;
    block[0] = (int16_t)(*pred << al);
}

static void jpeg_decode_block_progressive_dc_refine(QwJpegDecoder *d,
                                                    int16_t *block, int al)
{
    if (jpeg_get_bit(d)) block[0] |= (int16_t)(1 << al);
}

static void jpeg_decode_block_progressive_ac_first(QwJpegDecoder *d,
                                                   QwJpegComponent *comp,
                                                   int16_t *block, int ss,
                                                   int se, int al)
{
    int k = ss;
    if (d->eobrun > 0) {
        --d->eobrun;
        return;
    }
    if (k == 0) k = 1;
    while (k <= se) {
        int rs = jpeg_huff_decode(d, &d->ac[comp->ta]);
        int r = rs >> 4;
        int s = rs & 15;
        if (s == 0) {
            if (r != 15) {
                d->eobrun = 1 << r;
                if (r) d->eobrun += jpeg_get_bits(d, r);
                --d->eobrun;
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > se) break;
        block[jpeg_zigzag[k]] = (int16_t)(jpeg_extend(jpeg_get_bits(d, s), s) << al);
        ++k;
    }
}

static int jpeg_decode_block_progressive_ac_refine(QwJpegDecoder *d,
                                                    QwJpegComponent *comp,
                                                    int16_t *block, int ss,
                                                    int se, int al)
{
    int k = ss;
    int p1 = 1 << al;
    int m1 = -1 << al;
    if (d->eobrun > 0) {
        --d->eobrun;
        return 0;
    }
    if (k == 0) k = 1;
    while (k <= se) {
        int rs = jpeg_huff_decode(d, &d->ac[comp->ta]);
        int r = rs >> 4;
        int s = rs & 15;
        int newval = 0;
        if (s != 0) {
            if (s != 1) return -1;
            newval = jpeg_get_bit(d) ? p1 : m1;
        } else if (r != 15) {
            d->eobrun = 1 << r;
            if (r) d->eobrun += jpeg_get_bits(d, r);
            --d->eobrun;
            break;
        }
        while (k <= se) {
            int16_t *coef = &block[jpeg_zigzag[k]];
            if (*coef != 0) {
                if (jpeg_get_bit(d)) {
                    if ((*coef & p1) == 0)
                        *coef = (int16_t)(*coef + (*coef > 0 ? p1 : m1));
                }
            } else {
                if (r == 0) break;
                --r;
            }
            ++k;
        }
        if (s != 0) {
            if (k > se) return -1;
            block[jpeg_zigzag[k]] = (int16_t)newval;
        }
        ++k;
    }
    return 0;
}

static int jpeg_decode_block(QwJpegDecoder *d, QwJpegComponent *comp,
                             int16_t *block, int *pred, int ss, int se,
                             int ah, int al)
{
    if (!d->progressive) {
        if (!d->dc[comp->td].valid || !d->ac[comp->ta].valid) return -1;
        jpeg_decode_block_baseline(d, comp, block, pred);
        return 0;
    }
    if (ss == 0) {
        if (ah == 0) {
            if (!d->dc[comp->td].valid) return -1;
            jpeg_decode_block_progressive_dc_first(d, comp, block, pred, al);
        } else {
            jpeg_decode_block_progressive_dc_refine(d, block, al);
        }
        return 0;
    }
    if (!d->ac[comp->ta].valid) return -1;
    if (ah == 0)
        jpeg_decode_block_progressive_ac_first(d, comp, block, ss, se, al);
    else
        return jpeg_decode_block_progressive_ac_refine(d, comp, block, ss, se,
                                                       al);
    return 0;
}

/* Decode one scan after SOS. */
static int jpeg_decode_scan(QwJpegDecoder *d, int ss, int se, int ah, int al,
                            int nscan, const int *scan_comps)
{
    int pred[4] = { 0, 0, 0, 0 };
    int non_interleaved = nscan == 1;
    int total;
    int mcu;
    int i;

    if (non_interleaved) {
        QwJpegComponent *c = &d->comp[scan_comps[0]];
        total = c->comp_blocks_x * c->comp_blocks_y;
    } else {
        total = d->mcu_x * d->mcu_y;
    }

    for (mcu = 0; mcu < total; ++mcu) {
        if (d->restart_interval && mcu &&
            (mcu % d->restart_interval) == 0) {
            int b;
            d->bitcnt = 0;
            d->hit_marker = 0;
            do {
                b = jpeg_byte(d);
            } while (b == 0xff);
            if (b < 0xd0 || b > 0xd7) return -1;
            d->eobrun = 0;
            for (i = 0; i < 4; ++i) pred[i] = 0;
        }

        if (non_interleaved) {
            QwJpegComponent *comp = &d->comp[scan_comps[0]];
            int bx = mcu % comp->comp_blocks_x;
            int by = mcu / comp->comp_blocks_x;
            int16_t *block = comp->coeff +
                             ((size_t)by * (size_t)comp->blocks_x +
                              (size_t)bx) * 64u;
            if (jpeg_decode_block(d, comp, block, &pred[0], ss, se, ah, al) != 0)
                return -1;
            if (d->eof) return -1;
        } else {
            int bx = mcu % d->mcu_x;
            int by = mcu / d->mcu_x;
            for (i = 0; i < nscan; ++i) {
                QwJpegComponent *comp = &d->comp[scan_comps[i]];
                int v;
                int h;
                for (v = 0; v < comp->v; ++v) {
                    for (h = 0; h < comp->h; ++h) {
                        int bx2 = bx * comp->h + h;
                        int by2 = by * comp->v + v;
                        int16_t *block = comp->coeff +
                                         ((size_t)by2 * (size_t)comp->blocks_x +
                                          (size_t)bx2) * 64u;
                        if (jpeg_decode_block(d, comp, block, &pred[i], ss, se,
                                              ah, al) != 0)
                            return -1;
                    }
                }
            }
            if (d->eof) return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Output: IDCT + upsampling + colour conversion                       */
/* ------------------------------------------------------------------ */

static void jpeg_upsample_row(const QwJpegComponent *comp, int max_h, int max_v,
                              int y, int width, uint8_t *out)
{
    int hs = max_h / comp->h;
    int vs = max_v / comp->v;
    int sy = y / vs;
    const uint8_t *row;
    const uint8_t *near;
    const uint8_t *far;
    int x;
    int i;

    if (sy >= comp->plane_h) sy = comp->plane_h - 1;
    row = comp->plane + (size_t)sy * (size_t)comp->plane_w;

    if (hs == 1 && vs == 1) {
        for (x = 0; x < width; ++x) out[x] = row[x];
        return;
    }

    /* Vertical blending for 2x sampling. */
    if (vs == 2) {
        int sy2 = (y & 1) ? sy + 1 : sy - 1;
        if (sy2 < 0) sy2 = 0;
        if (sy2 >= comp->plane_h) sy2 = comp->plane_h - 1;
        near = row;
        far = comp->plane + (size_t)sy2 * (size_t)comp->plane_w;
        /* curr[x] = 3*near + far, computed on the fly below. */
    } else {
        near = row;
        far = row;
    }

    if (hs == 2) {
        int w = (width + 1) / 2;
        for (x = 0; x < w; ++x) {
            int cur = 3 * (int)near[x] + (int)far[x];
            int prev = (x == 0) ? cur : (3 * (int)near[x - 1] + (int)far[x - 1]);
            int next = (x + 1 < w) ? (3 * (int)near[x + 1] + (int)far[x + 1])
                                   : cur;
            int even = (3 * cur + prev + 8) >> 4;
            int odd = (3 * cur + next + 8) >> 4;
            if (vs == 2) {
                out[x * 2] = (uint8_t)(even & 0xff);
                if (x * 2 + 1 < width) out[x * 2 + 1] = (uint8_t)(odd & 0xff);
            } else {
                out[x * 2] = (uint8_t)even;
                if (x * 2 + 1 < width) out[x * 2 + 1] = (uint8_t)odd;
            }
        }
        return;
    }

    /* General fallback: nearest neighbour. */
    for (x = 0; x < width; ++x) {
        int sx = x * comp->h / max_h;
        if (sx >= comp->plane_w) sx = comp->plane_w - 1;
        if (vs == 2) {
            int cur = (3 * (int)row[sx] + (int)far[sx] + 2) >> 2;
            out[x] = (uint8_t)cur;
        } else {
            out[x] = row[sx];
        }
    }
    (void)i;
}

int qw_jpeg_decode(const uint8_t *data, size_t size, QwJpegImage *out_image)
{
    QwJpegDecoder d;
    int marker;
    int seen_sof = 0;
    uint8_t *row_y = NULL;
    uint8_t *row_cb = NULL;
    uint8_t *row_cr = NULL;
    int channels;
    uint8_t *pixels = NULL;
    size_t pixel_count;
    int y;
    int x;

    if (out_image) {
        out_image->pixels = NULL;
        out_image->size = 0;
        out_image->width = 0;
        out_image->height = 0;
        out_image->channels = 0;
    }
    if (!data || !out_image || size < 2) return -1;
    if (data[0] != 0xff || data[1] != 0xd8) return -1;

    memset(&d, 0, sizeof(d));
    d.data = data;
    d.size = size;
    d.pos = 2;

    for (;;) {
        int b;
        if (d.pos >= d.size) break;
        b = jpeg_byte(&d);
        if (b != 0xff) continue;
        do { b = jpeg_byte(&d); } while (b == 0xff);
        if (b < 0) break;
        marker = b;
        if (marker == 0xd8 || marker == 0x01) continue;
        if (marker == 0xd9) break;

        if (marker >= 0xe0 && marker <= 0xef) {
            size_t len;
            const uint8_t *seg = jpeg_segment(&d, &len);
            if (!seg) break;
            if (marker == 0xe0 && len >= 5u && memcmp(seg, "JFIF", 4u) == 0)
                d.saw_jfif = 1;
            if (marker == 0xee && len >= 12u && memcmp(seg, "Adobe", 5u) == 0) {
                d.saw_adobe = 1;
                d.adobe_transform = seg[11];
            }
            continue;
        }
        if (marker == 0xdb) {
            size_t len;
            const uint8_t *seg = jpeg_segment(&d, &len);
            if (!seg || jpeg_parse_dqt(&d, seg, len) != 0) goto fail;
            continue;
        }
        if (marker == 0xc4) {
            size_t len;
            const uint8_t *seg = jpeg_segment(&d, &len);
            if (!seg || jpeg_parse_dht(&d, seg, len) != 0) goto fail;
            continue;
        }
        if (marker == 0xdd) {
            size_t len;
            const uint8_t *seg = jpeg_segment(&d, &len);
            if (!seg || len != 2) goto fail;
            d.restart_interval = (seg[0] << 8) | seg[1];
            continue;
        }
        if (marker == 0xfe) {
            size_t len;
            if (!jpeg_segment(&d, &len)) break;
            continue;
        }
        if (marker == 0xc0 || marker == 0xc2) {
            size_t len;
            const uint8_t *seg = jpeg_segment(&d, &len);
            if (!seg || jpeg_parse_sof(&d, marker, seg, len) != 0) goto fail;
            seen_sof = 1;
            continue;
        }
        if (marker == 0xda) {
            size_t len;
            const uint8_t *seg = jpeg_segment(&d, &len);
            int nscan;
            int i;
            int ss;
            int se;
            int ah;
            int al;
            int comp_index[4];
            if (!seg || !seen_sof) goto fail;
            if (len < 4) goto fail;
            nscan = seg[0];
            if (nscan < 1 || nscan > d.ncomp) goto fail;
            if (len != (size_t)(4 + nscan * 2)) goto fail;
            for (i = 0; i < nscan; ++i) {
                int id = seg[1 + i * 2];
                int tables = seg[2 + i * 2];
                int j;
                comp_index[i] = -1;
                for (j = 0; j < d.ncomp; ++j)
                    if (d.comp[j].id == id) comp_index[i] = j;
                if (comp_index[i] < 0) goto fail;
                if ((tables >> 4) > 3 || (tables & 15) > 3) goto fail;
                d.comp[comp_index[i]].td = tables >> 4;
                d.comp[comp_index[i]].ta = tables & 15;
                if (!d.dc[d.comp[comp_index[i]].td].valid &&
                    !d.progressive)
                    goto fail;
                if (!d.ac[d.comp[comp_index[i]].ta].valid &&
                    !d.progressive)
                    goto fail;
            }
            ss = seg[1 + nscan * 2];
            se = seg[2 + nscan * 2];
            ah = seg[3 + nscan * 2] >> 4;
            al = seg[3 + nscan * 2] & 15;
            d.bitcnt = 0;
            d.hit_marker = 0;
            d.marker = 0;
            d.eobrun = 0;
            {
                int scan_rc = jpeg_decode_scan(&d, ss, se, ah, al, nscan, comp_index);
                if (scan_rc != 0) goto fail;
            }
            /* The entropy reader rewound the terminating marker; the outer
             * loop picks it up from the current position. */
            continue;
        }
        /* Unknown marker: skip its segment. */
        {
            size_t skipped;
            if (!jpeg_segment(&d, &skipped)) break;
        }
    }

    if (!seen_sof || d.ncomp < 1) goto fail;

    /* IDCT every block into component planes. */
    {
        int c;
        for (c = 0; c < d.ncomp; ++c) {
            QwJpegComponent *comp = &d.comp[c];
            int by;
            int bx;
            for (by = 0; by < comp->blocks_y; ++by) {
                for (bx = 0; bx < comp->blocks_x; ++bx) {
                    const int16_t *block = comp->coeff +
                                           ((size_t)by * (size_t)comp->blocks_x +
                                            (size_t)bx) * 64u;
                    jpeg_idct_block(block, d.qtable[comp->tq],
                                    comp->plane + (size_t)by * 8u *
                                                      (size_t)comp->plane_w +
                                        (size_t)bx * 8u,
                                    comp->plane_w);
                }
            }
        }
    }

    channels = d.ncomp >= 3 ? 3 : 1;
    pixel_count = (size_t)d.width * (size_t)d.height;
    pixels = (uint8_t *)malloc(pixel_count * (size_t)channels);
    if (!pixels) goto fail;

    row_y = (uint8_t *)malloc((size_t)d.width);
    row_cb = channels == 3 ? (uint8_t *)malloc((size_t)d.width) : NULL;
    row_cr = channels == 3 ? (uint8_t *)malloc((size_t)d.width) : NULL;
    if (!row_y || (channels == 3 && (!row_cb || !row_cr))) {
        free(row_y);
        free(row_cb);
        free(row_cr);
        free(pixels);
        goto fail;
    }

    {
        int rgb_direct = 0;
        if (d.ncomp == 3) {
            if (d.saw_adobe) {
                rgb_direct = d.adobe_transform == 0;
            } else if (!d.saw_jfif && d.comp[0].id == 'R' &&
                       d.comp[1].id == 'G' && d.comp[2].id == 'B') {
                rgb_direct = 1;
            }
        }
        for (y = 0; y < d.height; ++y) {
            jpeg_upsample_row(&d.comp[0], d.max_h, d.max_v, y, d.width, row_y);
        if (channels == 3) {
            jpeg_upsample_row(&d.comp[1], d.max_h, d.max_v, y, d.width, row_cb);
            jpeg_upsample_row(&d.comp[2], d.max_h, d.max_v, y, d.width, row_cr);
        }
        for (x = 0; x < d.width; ++x) {
            if (channels == 1) {
                pixels[(size_t)y * (size_t)d.width + (size_t)x] = row_y[x];
            } else if (rgb_direct) {
                uint8_t *dst = pixels +
                               ((size_t)y * (size_t)d.width + (size_t)x) * 3u;
                dst[0] = row_y[x];
                dst[1] = row_cb[x];
                dst[2] = row_cr[x];
            } else {
                int yy = row_y[x];
                int cb = row_cb[x] - 128;
                int cr = row_cr[x] - 128;
                int r = yy + (int)(1.402 * cr + 0.5);
                int g = yy - (int)(0.344136 * cb + 0.714136 * cr + 0.5);
                int b = yy + (int)(1.772 * cb + 0.5);
                uint8_t *dst = pixels + ((size_t)y * (size_t)d.width +
                                         (size_t)x) * 3u;
                if (r < 0) r = 0;
                else if (r > 255) r = 255;
                if (g < 0) g = 0;
                else if (g > 255) g = 255;
                if (b < 0) b = 0;
                else if (b > 255) b = 255;
                dst[0] = (uint8_t)r;
                dst[1] = (uint8_t)g;
                dst[2] = (uint8_t)b;
            }
            }
        }
    }

    free(row_y);
    free(row_cb);
    free(row_cr);
    out_image->pixels = pixels;
    out_image->size = pixel_count * (size_t)channels;
    out_image->width = d.width;
    out_image->height = d.height;
    out_image->channels = channels;
    {
        int c;
        for (c = 0; c < d.ncomp; ++c) {
            free(d.comp[c].coeff);
            free(d.comp[c].plane);
        }
    }
    return 0;

fail:
    {
        int c;
        for (c = 0; c < d.ncomp; ++c) {
            free(d.comp[c].coeff);
            free(d.comp[c].plane);
        }
    }
    free(row_y);
    free(row_cb);
    free(row_cr);
    free(pixels);
    return -1;
}

void qw_jpeg_image_free(QwJpegImage *image)
{
    if (!image) return;
    free(image->pixels);
    image->pixels = NULL;
    image->size = 0;
    image->width = 0;
    image->height = 0;
    image->channels = 0;
}
