/*
 * GC265 command processor for the q66 local experiment.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Execute the AP's VG Lite command stream against guest RAM. This subset
 * implements linear render targets, CLUT images, affine/perspective image
 * sampling and filled paths. Unsupported work reports a GPU error instead
 * of acknowledging a draw that never happened.
 */
#include <math.h>

typedef struct Q66GpuColor {
    unsigned r, g, b, a;
} Q66GpuColor;

typedef struct Q66GpuEdge {
    float x0, y0, x1, y1;
} Q66GpuEdge;

typedef struct Q66GpuCrossing {
    float x;
    int direction;
} Q66GpuCrossing;

static uint8_t *q66_gpu_ram(Q66MachineState *s, uint32_t address, size_t size)
{
    uint32_t offset;
    if (address >= 0x20000000 && address < 0x20160000) {
        offset = address - 0x20000000;
        if (size <= 0x160000 - offset) {
            return (uint8_t *)memory_region_get_ram_ptr(&s->sram) + offset;
        }
    }
    if ((address >= 0x38000000 && address < 0x38000000 + PSRAM_SIZE) ||
        (address >= 0x3c000000 && address < 0x3c000000 + PSRAM_SIZE)) {
        offset = address & (PSRAM_SIZE - 1);
        if (size <= PSRAM_SIZE - offset) {
            return (uint8_t *)memory_region_get_ram_ptr(&s->psram) + offset;
        }
    }
    return NULL;
}

static float q66_gpu_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* VG Lite solid API colors contain R in the low byte. */
static Q66GpuColor q66_gpu_color(uint32_t value)
{
    Q66GpuColor c = { value & 255, (value >> 8) & 255,
                      (value >> 16) & 255, value >> 24 };
    return c;
}

static unsigned q66_gpu_bpp(unsigned format)
{
    switch (format & 15) {
    case 0: case 6: case 7: return 1;
    case 1: case 4: return 2;
    case 9: case 10: return 3;
    case 2: case 3: return 4;
    default: return 0;
    }
}

static Q66GpuColor q66_gpu_unpack(const uint8_t *p, unsigned format)
{
    Q66GpuColor c = { 0, 0, 0, 255 };
    unsigned v, t;
    switch (format & 15) {
    case 0:
        c.r = c.g = c.b = 255; c.a = p[0];
        break;
    case 6:
        c.r = c.g = c.b = p[0];
        break;
    case 1: case 10:
        v = lduw_le_p(p);
        c.r = ((v >> 11) & 31) * 255 / 31;
        c.g = ((v >> 5) & 63) * 255 / 63;
        c.b = (v & 31) * 255 / 31;
        if ((format & 15) == 10) {
            c.a = p[(format & 0x10) ? 0 : 2];
            if (format & 0x10) {
                v = lduw_le_p(p + 1);
                c.r = ((v >> 11) & 31) * 255 / 31;
                c.g = ((v >> 5) & 63) * 255 / 63;
                c.b = (v & 31) * 255 / 31;
            }
        }
        break;
    case 2: case 3: case 9:
        if ((format & 0x10) && (format & 15) != 9) {
            c.r = p[0]; c.g = p[1]; c.b = p[2];
        } else {
            c.r = p[2]; c.g = p[1]; c.b = p[0];
        }
        if ((format & 15) == 3) {
            c.a = p[3];
        }
        break;
    case 4:
        v = lduw_le_p(p);
        if (format & 0x10) {
            c.r = (v & 15) * 17; c.g = ((v >> 4) & 15) * 17;
            c.b = ((v >> 8) & 15) * 17; c.a = (v >> 12) * 17;
        } else {
            c.a = (v & 15) * 17; c.b = ((v >> 4) & 15) * 17;
            c.g = ((v >> 8) & 15) * 17; c.r = (v >> 12) * 17;
        }
        break;
    case 7:
        v = p[0]; c.a = (v & 3) * 85; c.b = ((v >> 2) & 3) * 85;
        c.g = ((v >> 4) & 3) * 85; c.r = (v >> 6) * 85;
        break;
    default:
        break;
    }
    if (format & 0x20) {
        t = c.r; c.r = c.b; c.b = t;
    }
    return c;
}

static void q66_gpu_pack(uint8_t *p, unsigned format, Q66GpuColor c)
{
    unsigned t, v;
    if (format & 0x20) {
        t = c.r; c.r = c.b; c.b = t;
    }
    switch (format & 15) {
    case 0: p[0] = c.a; break;
    case 6: p[0] = (c.r * 77 + c.g * 150 + c.b * 29) >> 8; break;
    case 1: case 10:
        v = (c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3);
        if ((format & 15) == 10 && (format & 0x10)) {
            p[0] = c.a; stw_le_p(p + 1, v);
        } else {
            stw_le_p(p, v);
            if ((format & 15) == 10) {
                p[2] = c.a;
            }
        }
        break;
    case 2: case 3: case 9:
        if ((format & 0x10) && (format & 15) != 9) {
            p[0] = c.r; p[1] = c.g; p[2] = c.b;
        } else {
            p[0] = c.b; p[1] = c.g; p[2] = c.r;
        }
        if ((format & 15) != 9) {
            p[3] = (format & 15) == 3 ? c.a : 255;
        }
        break;
    case 4:
        if (format & 0x10) {
            v = (c.a >> 4) << 12 | (c.b >> 4) << 8 |
                (c.g >> 4) << 4 | (c.r >> 4);
        } else {
            v = (c.r >> 4) << 12 | (c.g >> 4) << 8 |
                (c.b >> 4) << 4 | (c.a >> 4);
        }
        stw_le_p(p, v);
        break;
    case 7:
        p[0] = (c.r >> 6) << 6 | (c.g >> 6) << 4 |
               (c.b >> 6) << 2 | (c.a >> 6);
        break;
    }
}

static uint8_t *q66_gpu_target(Q66MachineState *s, int x, int y)
{
    unsigned format = s->gpu_states[0xa10] & 0x3f;
    unsigned bpp = q66_gpu_bpp(format);
    uint32_t stride = s->gpu_states[0xa12] & 0xffff;
    if (bpp == 3) {
        stride = stride / 4 * 3;
    }
    if (!bpp || !stride || x < 0 || y < 0 ||
        x >= (s->gpu_states[0xa13] & 0xffff) ||
        y >= (s->gpu_states[0xa13] >> 16) ||
        (uint64_t)(x + 1) * bpp > stride) {
        return NULL;
    }
    return q66_gpu_ram(s, s->gpu_states[0xa11] + y * stride + x * bpp, bpp);
}

static bool q66_gpu_put(Q66MachineState *s, int x, int y, Q66GpuColor src,
                        unsigned coverage, bool premultiplied)
{
    unsigned format = s->gpu_states[0xa10] & 0x3f;
    unsigned blend = (s->gpu_states[0xa00] >> 8) & 15;
    unsigned alpha = s->gpu_states[0xad1] & 3;
    unsigned ga = (s->gpu_states[0xad1] >> 2) & 255;
    Q66GpuColor dst, out;
    uint8_t *p = q66_gpu_target(s, x, y);
    if (!p) {
        return false;
    }
    dst = q66_gpu_unpack(p, format);
    if (alpha) {
        unsigned old = src.a;
        src.a = alpha == 1 ? ga : (src.a * ga + 127) / 255;
        if (premultiplied) {
            src.r = old ? src.r * src.a / old : 0;
            src.g = old ? src.g * src.a / old : 0;
            src.b = old ? src.b * src.a / old : 0;
        }
    }
    if (blend == 0 && coverage == 255) {
        out = src;
    } else if (blend == 0 || blend == 1) {
        unsigned a = src.a * coverage / 255;
        unsigned factor = premultiplied ? coverage : a;
        out.r = MIN(255, (src.r * factor + dst.r * (255 - a) + 127) / 255);
        out.g = MIN(255, (src.g * factor + dst.g * (255 - a) + 127) / 255);
        out.b = MIN(255, (src.b * factor + dst.b * (255 - a) + 127) / 255);
        out.a = MIN(255, a + (dst.a * (255 - a) + 127) / 255);
    } else {
        return false;
    }
    q66_gpu_pack(p, format, out);
    s->gpu_pixels++;
    return true;
}

static bool q66_gpu_texel(Q66MachineState *s, int x, int y, Q66GpuColor *color)
{
    uint32_t fmt = s->gpu_states[0xa25];
    unsigned w = s->gpu_states[0xa2f] & 0xffff;
    unsigned h = s->gpu_states[0xa2f] >> 16;
    unsigned stride = s->gpu_states[0xa2b] & 0xffff;
    unsigned index_format = fmt & 0xe00, bpp, index;
    uint8_t *p;
    if (x < 0 || y < 0 || x >= w || y >= h) {
        *color = (Q66GpuColor) { 0, 0, 0, 0 };
        return true;
    }
    /* Packed RGB888/565+A8 reuse low format zero. Decode the high format
     * selector before L8, and undo the driver's 4-byte stride encoding. */
    if (fmt & 0x60000000) {
        bpp = 3;
        index = (fmt & 0x60000000) == 0x40000000 ? 10 : 9;
        stride = stride / 4 * 3;
    } else if (index_format && index_format <= 0x800) {
        unsigned bits = 1u << (index_format / 0x200 - 1);
        unsigned base = bits == 8 ? 0xb00 : bits == 4 ? 0xaa0 :
                        bits == 2 ? 0xa9c : 0xa98;
        p = q66_gpu_ram(s, s->gpu_states[0xa29] + y * stride + x * bits / 8, 1);
        if (!p) {
            return false;
        }
        index = (*p >> ((x * bits) & 7)) & ((1u << bits) - 1);
        /* The GC265 CLUT receives the LVGL BGRA palette unchanged. */
        index = s->gpu_states[base + index];
        *color = (Q66GpuColor) { (index >> 16) & 255, (index >> 8) & 255,
                                 index & 255, index >> 24 };
        return true;
    } else {
        switch (fmt & 15) {
        case 0: /* L8 */
            p = q66_gpu_ram(s, s->gpu_states[0xa29] + y * stride + x, 1);
            if (!p) { return false; }
            *color = (Q66GpuColor) { *p, *p, *p, 255 };
            return true;
        case 1: /* A4 */
        case 2: /* A8 */
            p = q66_gpu_ram(s, s->gpu_states[0xa29] + y * stride +
                            ((fmt & 15) == 1 ? x / 2 : x), 1);
            if (!p) { return false; }
            *color = q66_gpu_color(s->gpu_states[0xa02]);
            color->a = ((fmt & 15) == 1) ? ((*p >> ((x & 1) * 4)) & 15) * 17 : *p;
            return true;
        case 3: bpp = 2; index = 4; break; /* 4444 */
        case 5: bpp = 2; index = 1; break; /* 565 */
        case 6: bpp = 4; index = 2; break; /* X8888 */
        case 7: bpp = 4; index = 3; break; /* A8888 */
        default:
            return false;
        }
    }
    p = q66_gpu_ram(s, s->gpu_states[0xa29] + y * stride + x * bpp, bpp);
    if (!p) { return false; }
    *color = q66_gpu_unpack(p, index | (fmt & 0x30));
    return true;
}

static bool q66_gpu_sample_image(Q66MachineState *s, int x, int y,
                                  Q66GpuColor multiply, Q66GpuColor *color)
{
    float m[9], z, u, v;
    unsigned i, w = s->gpu_states[0xa2f] & 0xffff;
    unsigned h = s->gpu_states[0xa2f] >> 16;
    int sx, sy;
    uint32_t fmt = s->gpu_states[0xa25];
    if (!w || !h) { return false; }
    for (i = 0; i < 3; i++) {
        m[i] = q66_gpu_float(s->gpu_states[0xa18 + i]);
        m[3 + i] = q66_gpu_float(s->gpu_states[0xa1c + i]);
        m[6 + i] = q66_gpu_float(s->gpu_states[0xa20 + i]);
    }
    z = m[2] + x * m[5] + y * m[8];
    u = (m[0] + x * m[3] + y * m[6]) / z;
    v = (m[1] + x * m[4] + y * m[7]) / z;
    if (!isfinite(u) || !isfinite(v) || fabsf(u) > 65536 ||
        fabsf(v) > 65536) { return false; }
    sx = floorf(u * w); sy = floorf(v * h);
    if ((fmt & 0x01000000) && (sx < 0 || sy < 0 || sx >= w || sy >= h)) {
        if (fmt & 0x1000) {
            sx = CLAMP(sx, 0, (int)w - 1);
            sy = CLAMP(sy, 0, (int)h - 1);
        } else {
            *color = q66_gpu_color(s->gpu_states[0xa27]);
            return true;
        }
    }
    if (!q66_gpu_texel(s, sx, sy, color)) { return false; }
    if ((s->gpu_states[0xa00] & 0x3000) == 0x2000) {
        color->r = color->r * multiply.r / 255;
        color->g = color->g * multiply.g / 255;
        color->b = color->b * multiply.b / 255;
        color->a = color->a * multiply.a / 255;
    }
    return true;
}

static bool q66_gpu_rect(Q66MachineState *s, const uint8_t *data, unsigned len)
{
    int x, y, x0, y0, x1, y1;
    uint32_t control = s->gpu_states[0xa00];
    Q66GpuColor color = q66_gpu_color(s->gpu_states[0xa02]);
    bool image = control & 0x3000;
    float m[9];
    unsigned i;
    if (len != 8 || (control & 3) != 1 || (control & 0x40) ||
        (s->gpu_states[0xa12] & 0x10000000)) {
        return false;
    }
    x0 = (int16_t)lduw_le_p(data); y0 = (int16_t)lduw_le_p(data + 2);
    x1 = x0 + lduw_le_p(data + 4); y1 = y0 + lduw_le_p(data + 6);
    x0 = MAX(0, x0); y0 = MAX(0, y0);
    x1 = MIN(x1, s->gpu_states[0xa13] & 0xffff);
    y1 = MIN(y1, s->gpu_states[0xa13] >> 16);
    if (x1 > 2048 || y1 > 2048) { return false; }
    for (i = 0; i < 3; i++) {
        m[i] = q66_gpu_float(s->gpu_states[0xa18 + i]);
        m[3 + i] = q66_gpu_float(s->gpu_states[0xa1c + i]);
        m[6 + i] = q66_gpu_float(s->gpu_states[0xa20 + i]);
        if (image && (!isfinite(m[i]) || !isfinite(m[3 + i]) ||
                      !isfinite(m[6 + i]))) { return false; }
    }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            Q66GpuColor c = color;
            if (image) {
                float z = m[2] + x * m[5] + y * m[8];
                float u = (m[0] + x * m[3] + y * m[6]) / z;
                float v = (m[1] + x * m[4] + y * m[7]) / z;
                unsigned w = s->gpu_states[0xa2f] & 0xffff;
                unsigned h = s->gpu_states[0xa2f] >> 16;
                if (!isfinite(u) || !isfinite(v) || fabsf(u) > 65536 ||
                    fabsf(v) > 65536) { return false; }
                if (!q66_gpu_texel(s, floorf(u * w), floorf(v * h), &c)) {
                    return false;
                }
                if ((control & 0x3000) == 0x2000) {
                    c.r = c.r * color.r / 255; c.g = c.g * color.g / 255;
                    c.b = c.b * color.b / 255; c.a = c.a * color.a / 255;
                }
            }
            if (!q66_gpu_put(s, x, y, c, 255, image &&
                             (s->gpu_states[0xa25] & 0x100))) {
                return false;
            }
        }
    }
    return true;
}

static void q66_gpu_edge(Q66MachineState *s, GArray *edges,
                         float x0, float y0, float x1, float y1)
{
    float m[6];
    Q66GpuEdge edge;
    unsigned i;
    for (i = 0; i < 6; i++) {
        m[i] = q66_gpu_float(s->gpu_states[0xa40 + i]);
    }
    edge = (Q66GpuEdge) { m[0] * x0 + m[1] * y0 + m[2],
                         m[3] * x0 + m[4] * y0 + m[5],
                         m[0] * x1 + m[1] * y1 + m[2],
                         m[3] * x1 + m[4] * y1 + m[5] };
    g_array_append_val(edges, edge);
}

static int q66_gpu_cross_compare(const void *a, const void *b)
{
    float x = ((const Q66GpuCrossing *)a)->x;
    float y = ((const Q66GpuCrossing *)b)->x;
    return (x > y) - (x < y);
}

static bool q66_gpu_path(Q66MachineState *s, const uint8_t *data, unsigned len)
{
    g_autoptr(GArray) edges = g_array_new(false, false, sizeof(Q66GpuEdge));
    g_autofree Q66GpuCrossing *crossings = NULL;
    g_autofree float *coverage = NULL;
    unsigned fmt = (s->gpu_states[0xa34] >> 20) & 3;
    unsigned unit = fmt == 0 ? 1 : fmt == 1 ? 2 : 4;
    unsigned offset = 0, i, count, pass;
    float x = 0, y = 0, start_x = 0, start_y = 0;
    bool open = false, ended = false;
    int x0, y0, x1, y1, yy, xx;
    Q66GpuColor color = q66_gpu_color(s->gpu_states[0xa02]);
    bool image = s->gpu_states[0xa00] & 0x3000;
    /* Linear pattern paths share the image sampler with rectangle blits. */
    if ((s->gpu_states[0xa00] & 0x40) ||
        (s->gpu_states[0xa12] & 0x10000000)) {
        return false;
    }
    while (offset < len && edges->len < 8192) {
        unsigned op = data[offset++], n;
        float c[6];
        if (op == 0 || op == 1 || op == 2 || op == 3) {
            if (open) {
                q66_gpu_edge(s, edges, x, y, start_x, start_y);
                open = false;
            }
            if (op <= 1) {
                x = start_x; y = start_y;
                if (op == 0) { ended = true; break; }
                /* CLOSE has no coordinates, but still occupies a complete
                 * format unit. Its padding must not terminate the path. */
                offset = ROUND_UP(offset, unit);
                continue;
            }
        }
        if (op < 2 || op > 9) { return false; }
        n = op <= 5 ? 2 : op <= 7 ? 4 : 6;
        offset = ROUND_UP(offset, unit);
        if (offset + unit * n > len) { return false; }
        for (i = 0; i < n; i++, offset += unit) {
            c[i] = fmt == 0 ? (int8_t)data[offset] : fmt == 1 ?
                   (int16_t)lduw_le_p(data + offset) : fmt == 2 ?
                   (int32_t)ldl_le_p(data + offset) :
                   q66_gpu_float(ldl_le_p(data + offset));
            if (op & 1) { c[i] += (i & 1) ? y : x; }
            if (!isfinite(c[i]) || fabsf(c[i]) > 1e6f) { return false; }
        }
        if (op <= 3) {
            x = start_x = c[0]; y = start_y = c[1]; open = true;
        } else if (op <= 5) {
            q66_gpu_edge(s, edges, x, y, c[0], c[1]); x = c[0]; y = c[1];
        } else {
            float px = x, py = y;
            unsigned steps = 32;
            for (i = 1; i <= steps; i++) {
                float t = (float)i / steps, u = 1 - t, nx, ny;
                if (op <= 7) {
                    nx = u * u * x + 2 * u * t * c[0] + t * t * c[2];
                    ny = u * u * y + 2 * u * t * c[1] + t * t * c[3];
                } else {
                    nx = u * u * u * x + 3 * u * u * t * c[0] +
                         3 * u * t * t * c[2] + t * t * t * c[4];
                    ny = u * u * u * y + 3 * u * u * t * c[1] +
                         3 * u * t * t * c[3] + t * t * t * c[5];
                }
                q66_gpu_edge(s, edges, px, py, nx, ny); px = nx; py = ny;
            }
            x = c[n - 2]; y = c[n - 1];
        }
    }
    if (!ended) { return false; }
    x0 = MAX(0, (int16_t)s->gpu_states[0xa39]);
    y0 = MAX(0, (int16_t)(s->gpu_states[0xa39] >> 16));
    x1 = MIN(s->gpu_states[0xa13] & 0xffff,
             x0 + (s->gpu_states[0xa3a] & 0xffff));
    y1 = MIN(s->gpu_states[0xa13] >> 16, y0 + (s->gpu_states[0xa3a] >> 16));
    if (x1 > 2048 || y1 > 2048) { return false; }
    crossings = g_new(Q66GpuCrossing, MAX(1, edges->len));
    coverage = g_new0(float, MAX(1, x1));
    for (yy = y0; yy < y1; yy++) {
        memset(coverage, 0, x1 * sizeof(float));
        for (pass = 0; pass < 4; pass++) {
            float sy = yy + (pass + 0.5f) / 4, left = 0;
            int winding = 0;
            count = 0;
            for (i = 0; i < edges->len; i++) {
                Q66GpuEdge *e = &g_array_index(edges, Q66GpuEdge, i);
                if (sy >= MIN(e->y0, e->y1) && sy < MAX(e->y0, e->y1)) {
                    float at = e->x0 + (sy - e->y0) *
                               (e->x1 - e->x0) / (e->y1 - e->y0);
                    if (!isfinite(at)) { return false; }
                    crossings[count++] = (Q66GpuCrossing) { at, e->y1 > e->y0 ? 1 : -1 };
                }
            }
            qsort(crossings, count, sizeof(*crossings), q66_gpu_cross_compare);
            for (i = 0; i < count; i++) {
                int before = winding;
                winding = (s->gpu_states[0xa34] & 0x10) ? winding ^ 1 :
                          winding + crossings[i].direction;
                if (!before && winding) { left = crossings[i].x; }
                if (before && !winding) {
                    float right = MIN((float)x1, crossings[i].x);
                    left = MAX((float)x0, left);
                    for (xx = MAX(x0, (int)floorf(left)); xx < (int)ceilf(right); xx++) {
                        coverage[xx] += MAX(0.0f, MIN(right, xx + 1.0f) -
                                                 MAX(left, (float)xx));
                    }
                }
            }
        }
        for (xx = x0; xx < x1; xx++) {
            unsigned alpha = MIN(255, (unsigned)(coverage[xx] * 255 / 4 + 0.5f));
            Q66GpuColor c = color;
            if (alpha && image && !q66_gpu_sample_image(s, xx, yy, color, &c)) {
                return false;
            }
            if (alpha && !q66_gpu_put(s, xx, yy, c, alpha, image &&
                                      (s->gpu_states[0xa25] & 0x100))) {
                return false;
            }
        }
    }
    return true;
}

/* Optional, bounded host-side capture of the AP's actual draw inputs. */
static void q66_gpu_capture(Q66MachineState *s, const uint8_t *data, size_t len)
{
    const char *directory = g_getenv("Q66_GPU_CAPTURE_DIR");
    g_autofree char *name = NULL;
    g_autofree char *path = NULL;
    uint8_t *source;
    size_t bytes;
    uint8_t states[sizeof(s->gpu_states)];
    unsigned i;

    if (!directory || !*directory || s->gpu_draws >= 4096) {
        return;
    }
    for (i = 0; i < G_N_ELEMENTS(s->gpu_states); i++) {
        stl_le_p(states + i * 4, s->gpu_states[i]);
    }
    name = g_strdup_printf("draw-%04u-states.bin", s->gpu_draws);
    path = g_build_filename(directory, name, NULL);
    g_file_set_contents(path, (const char *)states, sizeof(states), NULL);
    g_free(name); g_free(path);
    name = g_strdup_printf("draw-%04u-data.bin", s->gpu_draws);
    path = g_build_filename(directory, name, NULL);
    g_file_set_contents(path, (const char *)data, len, NULL);
    if (!(s->gpu_states[0xa00] & 0x3000)) {
        return;
    }
    bytes = (s->gpu_states[0xa2b] & 0xffff) *
            (size_t)(s->gpu_states[0xa2f] >> 16);
    source = bytes && bytes <= 4 * MiB ?
             q66_gpu_ram(s, s->gpu_states[0xa29], bytes) : NULL;
    if (source) {
        g_free(name); g_free(path);
        name = g_strdup_printf("draw-%04u-source.bin", s->gpu_draws);
        path = g_build_filename(directory, name, NULL);
        g_file_set_contents(path, (const char *)source, bytes, NULL);
    }
}

static bool q66_gpu_commands(Q66MachineState *s, uint32_t address,
                             uint32_t bytes, unsigned depth, uint32_t *events)
{
    const uint8_t *buffer = q66_gpu_ram(s, address, bytes);
    unsigned offset = 0;
    if (!buffer || bytes > MiB || depth > 8) { return false; }
    while (offset + 8 <= bytes) {
        uint32_t command = ldl_le_p(buffer + offset);
        uint32_t arg = ldl_le_p(buffer + offset + 4);
        unsigned op = command >> 28;
        unsigned count, state, i;
        if (op == 0) {
            *events |= 1u << (command & 31);
            return true;
        } else if (op == 3) {
            count = (command >> 16) & 0xfff; state = command & 0xffff;
            if (!count || state + count > G_N_ELEMENTS(s->gpu_states) ||
                offset + ROUND_UP(count + 1, 2) * 4 > bytes) { return false; }
            for (i = 0; i < count; i++) {
                s->gpu_states[state + i] = ldl_le_p(buffer + offset + 4 + i * 4);
            }
            offset += ROUND_UP(count + 1, 2) * 4;
            continue;
        } else if (op == 4) {
            bool ok;
            count = command & 0xffff;
            if (!count || offset + 8 + count * 8 > bytes) { return false; }
            q66_gpu_capture(s, buffer + offset + 8, count * 8);
            if (s->gpu_states[0xa34] & 0x1000000) {
                ok = q66_gpu_path(s, buffer + offset + 8, count * 8);
            } else {
                ok = q66_gpu_rect(s, buffer + offset + 8, count * 8);
            }
            if (s->gpu_draws < 80 || !ok) {
                qemu_log_mask(LOG_UNIMP,
                    "q66 GPU draw=%u target=%08x fmt=%x size=%08x "
                    "control=%08x srcfmt=%08x data=%u ok=%u\n",
                    s->gpu_draws, s->gpu_states[0xa11], s->gpu_states[0xa10],
                    s->gpu_states[0xa13], s->gpu_states[0xa00],
                    s->gpu_states[0xa25], count * 8, ok);
            }
            if (!ok) { return false; }
            s->gpu_draws++;
            offset += 8 + count * 8;
            continue;
        } else if (op == 6) {
            if (!q66_gpu_commands(s, arg, (command & 0xffff) * 8, depth + 1, events)) {
                return false;
            }
        } else if (op == 7) {
            return depth > 0;
        } else if (op != 1 && op != 2 && op != 8) {
            qemu_log_mask(LOG_UNIMP, "q66 GPU unknown command @%08x=%08x\n",
                          address + offset, command);
            return false;
        }
        offset += 8;
    }
    return false;
}

static void q66_gpu_update_irq(Q66MachineState *s)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->arm), 70),
                 !!(s->gpu_irq_status & q66_reg(s, 0x40220014)));
}

static void q66_gpu_run(void *opaque)
{
    Q66MachineState *s = opaque;
    uint32_t events = 0;
    bool ok = q66_gpu_commands(s, s->gpu_command_address,
                               s->gpu_command_bytes, 0, &events);
    if (!ok) { s->gpu_errors++; }
    s->gpu_irq_status |= ok ? events : 0x80000000;
    memory_region_set_dirty(&s->psram, 0, PSRAM_SIZE);
    memory_region_set_dirty(&s->sram, 0, 0x160000);
    if (s->gpu_submits < 32 || !ok) {
        qemu_log_mask(LOG_UNIMP,
            "q66 GPU submit=%u address=%08x bytes=%u draws=%u pixels=%u "
            "status=%08x ok=%u\n", s->gpu_submits, s->gpu_command_address,
            s->gpu_command_bytes, s->gpu_draws, s->gpu_pixels,
            s->gpu_irq_status, ok);
    }
    q66_gpu_update_irq(s);
}

static void q66_gpu_reset(Q66MachineState *s)
{
    timer_del(s->gpu_timer);
    memset(s->gpu_states, 0, sizeof(s->gpu_states));
    s->gpu_irq_status = 0;
    q66_gpu_update_irq(s);
}
