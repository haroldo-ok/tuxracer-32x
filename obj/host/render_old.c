/*
 * Tux Racer 32X - software rasteriser.
 *
 * Flat-shaded triangles into an 8bpp 320x224 buffer.  Spans are written
 * 16 bits at a time (the 32X framebuffer ignores byte writes of zero and
 * word writes are twice as fast anyway).  Triangles are queued, sorted by
 * depth (radix sort on a 12-bit key) and drawn back to front.
 */
#include "render.h"
#include "font8.h"

camera_t g_cam;
render_state_t *g_rs;

/* byte lanes of a 16-bit framebuffer word: the left pixel is the high
   byte on the big-endian SH-2, the low byte on a little-endian host. */
#ifdef __32X__
#define LEFT_SHIFT 8
#define RIGHT_SHIFT 0
#else
#define LEFT_SHIFT 0
#define RIGHT_SHIFT 8
#endif
#define LEFT_MASK ((u16)(0xff << LEFT_SHIFT))
#define RIGHT_MASK ((u16)(0xff << RIGHT_SHIFT))

void render_init(render_state_t *rs, u8 *fb)
{
    g_rs = rs;
    rs->fb = fb;
    rs->num_tris = 0;
    g_cam.far_z = FX(75.0);
}

void render_set_camera(const vec3 *pos, const vec3 *dir, const vec3 *up)
{
    /* setup_view_matrix: z = -dir, x = up cross z, y = z cross x; the
       view matrix rows are x, y, z (world->camera).  Camera looks down -z
       in that basis; we store +z forward by negating the z row so that
       camera-space z > 0 is in front. */
    vec3 x, y, z;
    z.x = -dir->x; z.y = -dir->y; z.z = -dir->z;
    v3_normalize(&z);
    v3_cross(&x, up, &z);
    v3_normalize(&x);
    v3_cross(&y, &z, &x);
    g_cam.pos = *pos;
    g_cam.view.m[0][0] = x.x; g_cam.view.m[0][1] = x.y; g_cam.view.m[0][2] = x.z;
    g_cam.view.m[1][0] = y.x; g_cam.view.m[1][1] = y.y; g_cam.view.m[1][2] = y.z;
    g_cam.view.m[2][0] = -z.x; g_cam.view.m[2][1] = -z.y; g_cam.view.m[2][2] = -z.z;
    g_cam.view.t.x = g_cam.view.t.y = g_cam.view.t.z = 0;
}

void render_begin_frame(void)
{
    g_rs->num_tris = 0;
}

int render_project(const vec3 *c, int *sx, int *sy)
{
    fx inv;
    if (c->z < NEAR_Z) return 0;
    /* inv = FOCAL / z  (both 16.16) -> 16.16 */
    inv = fxdiv(FOCAL, c->z);
    *sx = (SCREEN_W / 2) + FX_INT(fxmul(c->x, inv) + FX_HALF);
    *sy = (SCREEN_H / 2) - FX_INT(fxmul(c->y, inv) + FX_HALF);
    return 1;
}

void render_push_tri(int x0, int y0, int x1, int y1, int x2, int y2, u8 color, fx depth)
{
    render_state_t *rs = g_rs;
    stri_t *t;
    u32 key;
    if (rs->num_tris >= MAX_TRIS) return;
    t = &rs->tris[rs->num_tris];
    t->x0 = (s16)iclamp(x0, -2048, 2047); t->y0 = (s16)iclamp(y0, -2048, 2047);
    t->x1 = (s16)iclamp(x1, -2048, 2047); t->y1 = (s16)iclamp(y1, -2048, 2047);
    t->x2 = (s16)iclamp(x2, -2048, 2047); t->y2 = (s16)iclamp(y2, -2048, 2047);
    t->color = color;
    t->flags = 0;
    /* key: 12 bits, quasi-logarithmic depth: 1/128 m steps below 8 m
       (0..1023), 1/16 m steps 8..200 m (1024..4095) */
    if (depth < FX(8.0)) key = (u32)(depth >> 9);
    else key = 1024 + (u32)((depth - FX(8.0)) >> 12);
    if (key > 4095) key = 4095;
    t->key = (u16)key;
    rs->num_tris++;
}

/* clip a camera-space triangle against z = NEAR_Z, producing up to 4 verts */
static int clip_near(const vec3 *in, int n, vec3 *out)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const vec3 *a = &in[i], *b = &in[(i + 1) % n];
        int ina = a->z >= NEAR_Z, inb = b->z >= NEAR_Z;
        if (ina) out[m++] = *a;
        if (ina != inb) {
            fx t = fxdiv(NEAR_Z - a->z, b->z - a->z);
            out[m].x = a->x + fxmul(b->x - a->x, t);
            out[m].y = a->y + fxmul(b->y - a->y, t);
            out[m].z = NEAR_Z;
            m++;
        }
    }
    return m;
}

void render_tri_cam(const vec3 *a, const vec3 *b, const vec3 *c, u8 color, int cull)
{
    vec3 in[3], out[5];
    int n, i, v[5];
    fx depth;
    int sx[5], sy[5];

    /* trivial reject: all behind near or all beyond far */
    if (a->z < NEAR_Z && b->z < NEAR_Z && c->z < NEAR_Z) return;
    if (a->z > g_cam.far_z && b->z > g_cam.far_z && c->z > g_cam.far_z) return;

    in[0] = *a; in[1] = *b; in[2] = *c;
    if (a->z >= NEAR_Z && b->z >= NEAR_Z && c->z >= NEAR_Z) {
        n = 3;
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
    } else {
        n = clip_near(in, 3, out);
        if (n < 3) return;
    }
    for (i = 0; i < n; i++) {
        if (!render_project(&out[i], &sx[i], &sy[i])) return;
    }
    /* frustum reject on screen bounds */
    {
        int minx = sx[0], maxx = sx[0], miny = sy[0], maxy = sy[0];
        for (i = 1; i < n; i++) {
            if (sx[i] < minx) minx = sx[i]; if (sx[i] > maxx) maxx = sx[i];
            if (sy[i] < miny) miny = sy[i]; if (sy[i] > maxy) maxy = sy[i];
        }
        if (maxx < 0 || minx >= SCREEN_W || maxy < 0 || miny >= SCREEN_H) return;
    }
    /* backface cull in screen space (counter-clockwise = front) */
    if (cull) {
        int area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (area >= 0) return;
    }
    /* depth key: centroid z of the (unclipped) triangle */
    depth = (a->z + b->z + c->z) / 3;
    if (depth < NEAR_Z) depth = NEAR_Z;
    (void)v;
    render_push_tri(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], color, depth);
    if (n == 4) render_push_tri(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3], color, depth);
}

void render_tri_world(const vec3 *a, const vec3 *b, const vec3 *c, u8 color, int cull)
{
    vec3 ca, cb, cc;
    cam_transform(&ca, a);
    cam_transform(&cb, b);
    cam_transform(&cc, c);
    render_tri_cam(&ca, &cb, &cc, color, cull);
}

/* ------------------------------------------------------------ raster */

/* fill a horizontal span [x0, x1) on row y with colour c using 16-bit writes */
static inline void span(u8 *fb, int y, int x0, int x1, u8 c)
{
    u8 *row;
    u16 *p16;
    u16 cc;
    int n;
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (x0 >= x1) return;
    row = fb + y * SCREEN_W;
    cc = (u16)((c << 8) | c);
    if (x0 & 1) {
        /* odd start pixel shares a word with x0-1: keep the left byte
           (the left pixel is the high byte on the big-endian SH-2) */
        p16 = (u16 *)(row + x0 - 1);
        *p16 = (u16)((*p16 & LEFT_MASK) | (c << RIGHT_SHIFT));
        x0++;
        if (x0 >= x1) return;
    }
    if (x1 & 1) {
        /* odd end: last pixel x1-1 is the left byte of its word */
        p16 = (u16 *)(row + x1 - 1);
        *p16 = (u16)((*p16 & RIGHT_MASK) | (c << LEFT_SHIFT));
        x1--;
        if (x0 >= x1) return;
    }
    p16 = (u16 *)(row + x0);
    n = (x1 - x0) >> 1;
    while (n >= 4) { p16[0] = cc; p16[1] = cc; p16[2] = cc; p16[3] = cc; p16 += 4; n -= 4; }
    while (n-- > 0) *p16++ = cc;
}

void draw_hline(int x0, int x1, int y, u8 color)
{
    if (y < 0 || y >= SCREEN_H) return;
    span(g_rs->fb, y, x0, x1 + 1, color);
}

void fill_rect(int x, int y, int w, int h, u8 color)
{
    int yy;
    int x1 = x + w;
    if (y < 0) { h += y; y = 0; }
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    for (yy = y; yy < y + h; yy++) span(g_rs->fb, yy, x, x1, color);
}

/* Flat triangle: scanline conversion with 16.16 edge stepping.
   Pixels whose centre (x+0.5, y+0.5) lies inside the triangle are filled;
   spans are [ceil(xl-0.5), ceil(xr-0.5)) so shared edges never overlap or
   leave gaps. */
void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, u8 color)
{
    u8 *fb = g_rs->fb;
    int t, y;
    fx dx02, dx01, dx12;
    fx xl, xr;
    int ystart, yend;

    if (y1 < y0) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (y2 < y0) { t = x0; x0 = x2; x2 = t; t = y0; y0 = y2; y2 = t; }
    if (y2 < y1) { t = x1; x1 = x2; x2 = t; t = y1; y1 = y2; y2 = t; }
    if (y2 == y0) return;
    if (y2 <= 0 || y0 >= SCREEN_H) return;
    if (y2 - y0 > 4096) return;

    dx02 = fxdiv(FX_FROM_INT(x2 - x0), FX_FROM_INT(y2 - y0));
    /* is the middle vertex on the left or the right of the long edge? */
    {
        fx xm = FX_FROM_INT(x0) + fxmul(dx02, FX_FROM_INT(y1 - y0));
        int mid_left = FX_FROM_INT(x1) < xm;
        /* upper half: y0..y1 */
        if (y1 > y0) {
            dx01 = fxdiv(FX_FROM_INT(x1 - x0), FX_FROM_INT(y1 - y0));
            ystart = y0 < 0 ? 0 : y0;
            yend = y1 > SCREEN_H ? SCREEN_H : y1;
            /* x at scanline centre y+0.5 */
            xl = FX_FROM_INT(x0) + fxmul(mid_left ? dx01 : dx02, FX_FROM_INT(ystart - y0) + FX_HALF);
            xr = FX_FROM_INT(x0) + fxmul(mid_left ? dx02 : dx01, FX_FROM_INT(ystart - y0) + FX_HALF);
            for (y = ystart; y < yend; y++) {
                int l = FX_INT(xl + FX_HALF), r = FX_INT(xr + FX_HALF);
                span(fb, y, l, r, color);
                xl += mid_left ? dx01 : dx02;
                xr += mid_left ? dx02 : dx01;
            }
        }
        /* lower half: y1..y2 */
        if (y2 > y1) {
            dx12 = fxdiv(FX_FROM_INT(x2 - x1), FX_FROM_INT(y2 - y1));
            ystart = y1 < 0 ? 0 : y1;
            yend = y2 > SCREEN_H ? SCREEN_H : y2;
            if (mid_left) {
                xl = FX_FROM_INT(x1) + fxmul(dx12, FX_FROM_INT(ystart - y1) + FX_HALF);
                xr = FX_FROM_INT(x0) + fxmul(dx02, FX_FROM_INT(ystart - y0) + FX_HALF);
            } else {
                xl = FX_FROM_INT(x0) + fxmul(dx02, FX_FROM_INT(ystart - y0) + FX_HALF);
                xr = FX_FROM_INT(x1) + fxmul(dx12, FX_FROM_INT(ystart - y1) + FX_HALF);
            }
            for (y = ystart; y < yend; y++) {
                int l = FX_INT(xl + FX_HALF), r = FX_INT(xr + FX_HALF);
                span(fb, y, l, r, color);
                xl += mid_left ? dx12 : dx02;
                xr += mid_left ? dx02 : dx12;
            }
        }
    }
}

/* ------------------------------------------------------------ flush */

static u16 count0[256];
static u16 count1[16];

void render_flush(void)
{
    render_state_t *rs = g_rs;
    int n = rs->num_tris;
    int i;
    u16 *tmp_order = rs->tmp_order;
    /* two-pass LSD radix sort on 12-bit key (8 + 4 bits), descending depth */
    for (i = 0; i < 256; i++) count0[i] = 0;
    for (i = 0; i < 16; i++) count1[i] = 0;
    for (i = 0; i < n; i++) {
        u32 k = rs->tris[i].key;
        count0[k & 0xff]++;
        count1[(k >> 8) & 0xf]++;
    }
    {
        u16 sum = 0;
        for (i = 0; i < 256; i++) { u16 c = count0[i]; count0[i] = sum; sum += c; }
        sum = 0;
        for (i = 0; i < 16; i++) { u16 c = count1[i]; count1[i] = sum; sum += c; }
    }
    for (i = 0; i < n; i++) tmp_order[count0[rs->tris[i].key & 0xff]++] = (u16)i;
    for (i = 0; i < n; i++) { u16 t = tmp_order[i]; rs->order[count1[(rs->tris[t].key >> 8) & 0xf]++] = t; }
    /* draw far to near */
    for (i = n - 1; i >= 0; i--) {
        const stri_t *t = &rs->tris[rs->order[i]];
        fill_tri(t->x0, t->y0, t->x1, t->y1, t->x2, t->y2, t->color);
    }
}

/* ------------------------------------------------------------- text */

int text_width(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n * 8;
}

void draw_text(int x, int y, const char *s, u8 color)
{
    u8 *fb = g_rs->fb;
    for (; *s; s++, x += 8) {
        int ch = (u8)*s;
        int r, c;
        const u8 *g;
        if (ch < 32 || ch > 127) ch = '?';
        g = font8[ch - 32];
        if (x < 0 || x + 8 > SCREEN_W) continue;
        for (r = 0; r < 8; r++) {
            u8 bits = g[r];
            u8 *row;
            int yy = y + r;
            if (yy < 0 || yy >= SCREEN_H || !bits) continue;
            row = fb + yy * SCREEN_W + x;
            /* write pairs to keep 16-bit access */
            for (c = 0; c < 8; c += 2) {
                int b0 = (bits >> (7 - c)) & 1, b1 = (bits >> (6 - c)) & 1;
                if (b0 | b1) {
                    u16 *p = (u16 *)(row + c);
                    u16 v = *p;
                    if (b0) v = (u16)((v & RIGHT_MASK) | (color << LEFT_SHIFT));
                    if (b1) v = (u16)((v & LEFT_MASK) | (color << RIGHT_SHIFT));
                    *p = v;
                }
            }
        }
    }
}

void draw_text_big(int x, int y, const char *s, u8 color)
{
    u8 *fb = g_rs->fb;
    for (; *s; s++, x += 16) {
        int ch = (u8)*s;
        int r, c;
        const u8 *g;
        if (ch < 32 || ch > 127) ch = '?';
        g = font8[ch - 32];
        if (x < 0 || x + 16 > SCREEN_W) continue;
        for (r = 0; r < 16; r++) {
            u8 bits = g[r >> 1];
            u8 *row;
            int yy = y + r;
            if (yy < 0 || yy >= SCREEN_H || !bits) continue;
            row = fb + yy * SCREEN_W + x;
            for (c = 0; c < 8; c++) {
                if ((bits >> (7 - c)) & 1) {
                    u16 *p = (u16 *)(row + c * 2);
                    *p = (u16)((color << 8) | color);
                }
            }
        }
    }
}

void draw_number(int x, int y, int n, int digits, u8 color)
{
    char buf[12];
    int i;
    if (n < 0) n = 0;
    buf[digits] = 0;
    for (i = digits - 1; i >= 0; i--) {
        buf[i] = (char)('0' + n % 10);
        n /= 10;
    }
    draw_text(x, y, buf, color);
}
