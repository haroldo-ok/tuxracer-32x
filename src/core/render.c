/*
 * Tux Racer 32X - software rasteriser.
 *
 * Flat-shaded triangles into an 8bpp 320x224 buffer.  Spans are written
 * 16 bits at a time (the 32X framebuffer ignores byte writes of zero and
 * word writes are twice as fast anyway).  Triangles are queued, sorted by
 * depth (radix sort on a 12-bit key) and drawn back to front.
 */
#include <stdint.h>
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

/* 32X: partial words are written through the framebuffer's overwrite
   image, where zero bytes are not stored (no read-modify-write needed). */
#define FB_OVR 0x20000
/* Glyph rows are expanded through a 16-entry table (one nibble = 4
   pixels = one 32-bit word: 0xff where the bit is set) and written as
   masked 32-bit words.  On the 32X the write goes through the overwrite
   image, where zero bytes leave the background alone; on the host it is a
   read-modify-write. */
static const u32 nib_mask[16] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff
};

/* masks above are in "leftmost pixel = most significant byte" order,
   which is the memory order on the big-endian SH-2; the little-endian
   host needs the bytes swapped */
#ifdef __32X__
#define PUT_MASKED32(p, m, c32) (*(volatile u32 *)((u8 *)(p) + FB_OVR) = (m) & (c32))
#else
static inline u32 bswap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }
#define PUT_MASKED32(p, m, c32) do { u32 mm_ = bswap32(m); *(u32 *)(p) = (*(u32 *)(p) & ~mm_) | (mm_ & (c32)); } while (0)
#endif


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
    if (c->z < NEAR_Z) return 0;
    project_fast(c->x, c->y, c->z, sx, sy);
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
    key = depth_key(depth);
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
    fx depth;
    fx far = g_cam.far_z;

    /* trivial reject: all beyond far */
    if (a->z > far && b->z > far && c->z > far) return;

    if (a->z >= NEAR_Z && b->z >= NEAR_Z && c->z >= NEAR_Z) {
        /* common case: no clipping, project straight into the queue */
        int x0, y0, x1, y1, x2, y2;
        project_fast(a->x, a->y, a->z, &x0, &y0);
        project_fast(b->x, b->y, b->z, &x1, &y1);
        project_fast(c->x, c->y, c->z, &x2, &y2);
        if ((x0 < 0 && x1 < 0 && x2 < 0) || (x0 >= SCREEN_W && x1 >= SCREEN_W && x2 >= SCREEN_W)) return;
        if ((y0 < 0 && y1 < 0 && y2 < 0) || (y0 >= SCREEN_H && y1 >= SCREEN_H && y2 >= SCREEN_H)) return;
        if (cull && (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) >= 0) return;
        depth = (a->z + b->z + c->z) / 3;
        render_push_tri(x0, y0, x1, y1, x2, y2, color, depth);
        return;
    }
    /* all behind near */
    if (a->z < NEAR_Z && b->z < NEAR_Z && c->z < NEAR_Z) return;
    {
        vec3 in[3], out[5];
        int n, i;
        int sx[5], sy[5];
        in[0] = *a; in[1] = *b; in[2] = *c;
        n = clip_near(in, 3, out);
        if (n < 3) return;
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
        render_push_tri(sx[0], sy[0], sx[1], sy[1], sx[2], sy[2], color, depth);
        if (n == 4) render_push_tri(sx[0], sy[0], sx[2], sy[2], sx[3], sy[3], color, depth);
    }
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

/* fill a horizontal span [x0, x1) on row y with colour c: whole 32-bit
   words, the partial end words masked (overwrite image on the 32X, RMW on
   the host).  Colour 0 cannot be drawn this way on the 32X (zero bytes
   are dropped by the overwrite image), so black falls back to 16-bit
   writes with real read-modify-write of the neighbours. */
static const u32 span_lmask[4] = { 0xffffffff, 0x00ffffff, 0x0000ffff, 0x000000ff };
static const u32 span_rmask[4] = { 0xffffffff, 0xff000000, 0xffff0000, 0xffffff00 };

static void span_slow(u8 *row, int x0, int x1, u8 c)
{
    u16 *p16;
    u16 cc = (u16)((c << 8) | c);
    int n;
    if (x0 & 1) {
        p16 = (u16 *)(row + x0 - 1);
        *p16 = (u16)((*p16 & LEFT_MASK) | (c << RIGHT_SHIFT));
        x0++;
        if (x0 >= x1) return;
    }
    if (x1 & 1) {
        p16 = (u16 *)(row + x1 - 1);
        *p16 = (u16)((*p16 & RIGHT_MASK) | (c << LEFT_SHIFT));
        x1--;
        if (x0 >= x1) return;
    }
    p16 = (u16 *)(row + x0);
    n = (x1 - x0) >> 1;
    while (n-- > 0) *p16++ = cc;
}

static inline void span(u8 *fb, int y, int x0, int x1, u8 c)
{
    u8 *row;
    u32 *p0, *p1;
    u32 c32, m0, m1;
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (x0 >= x1) return;
    row = fb + y * SCREEN_W;
    if (c == 0) { span_slow(row, x0, x1, c); return; }
    c32 = ((u32)c << 24) | ((u32)c << 16) | ((u32)c << 8) | c;
    m0 = span_lmask[x0 & 3];
    m1 = span_rmask[x1 & 3];
    p0 = (u32 *)(row + (x0 & ~3));
    p1 = (u32 *)(row + ((x1 - 1) & ~3));
    if (p0 == p1) {
        PUT_MASKED32(p0, m0 & m1, c32);
        return;
    }
    PUT_MASKED32(p0, m0, c32);
    PUT_MASKED32(p1, m1, c32);
    p0++;
    while (p0 < p1) *p0++ = c32;
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

/* ------------------------------------------------------- triangles */

/* Edge slopes come from a reciprocal table (no divider latency on the
   SH-2): slope16 = (dx << 16) * (2^30 / dy) >> 30. */
static inline fx edge_slope(int dx, int dy)
{
    if (dy < 1024) return mul_shr30(dx << 16, recip30_tab[dy]);
    return fxdiv(FX_FROM_INT(dx), FX_FROM_INT(dy));
}

/* Odd edge pixels are written with 16-bit accesses: through the 32X
   framebuffer's overwrite image on the console (a zero byte leaves the
   neighbour untouched, no read needed), read-modify-write on the host. */
#ifdef __32X__
#define PUT_LEFT_PIXEL(p, c)  (*(volatile u16 *)((u8 *)(p) + FB_OVR) = (u16)((c) << 8))
#define PUT_RIGHT_PIXEL(p, c) (*(volatile u16 *)((u8 *)(p) + FB_OVR) = (u16)(c))
#else
#define PUT_LEFT_PIXEL(p, c)  (*(u16 *)(p) = (u16)((*(u16 *)(p) & RIGHT_MASK) | ((c) << LEFT_SHIFT)))
#define PUT_RIGHT_PIXEL(p, c) (*(u16 *)(p) = (u16)((*(u16 *)(p) & LEFT_MASK) | ((c) << RIGHT_SHIFT)))
#endif

/* C scanline filler (host build, single triangles on the 32X, and the
   reference the bench ROM checks src/platform/32x/raster.s against):
   rows1 scanlines with edges (xl,dl)/(xr,dr), then the left (mid_left) or
   right edge is replaced by (xm,dm) for rows2 more.  Span = [xl>>16, xr>>16)
   clipped to the screen width. */
void raster_tri_c(raster_params_t *rp)
{
    u8 *row = rp->row;
    fx xl = rp->xl, dl = rp->dl, xr = rp->xr, dr = rp->dr;
    int rows = rp->rows1;
    u16 cc = (u16)rp->cc;
    int c = cc & 0xff;
    int phase;
    for (phase = 0; phase < 2; phase++) {
        while (rows-- > 0) {
            int l = xl >> 16, r = xr >> 16;
            if (l < 0) l = 0;
            if (r > SCREEN_W) r = SCREEN_W;
            if (l < r) {
                int n = r - l;
                u16 *p;
                if (l & 1) { PUT_RIGHT_PIXEL(row + l - 1, c); l++; n--; }
                if (n & 1) { PUT_LEFT_PIXEL(row + l + n - 1, c); n--; }
                p = (u16 *)(row + l);
                n >>= 1;
                if (n > 0 && ((uintptr_t)p & 2)) { *p++ = cc; n--; }
                {
                    u32 *q = (u32 *)p;
                    u32 cc32 = ((u32)cc << 16) | cc;
                    while (n >= 2) { *q++ = cc32; n -= 2; }
                    p = (u16 *)q;
                }
                if (n > 0) *p = cc;
            }
            xl += dl; xr += dr; row += SCREEN_W;
        }
        rows = rp->rows2;
        rp->rows2 = 0;
        if (rp->mid_left) { xl = rp->xm; dl = rp->dm; }
        else { xr = rp->xm; dr = rp->dm; }
    }
}


/* Flat triangle: scanline conversion with 16.16 edge stepping.  Pixels
   whose centre (x+0.5, y+0.5) lies inside the triangle are filled, so
   shared edges never overlap or gap.  The three vertices are sorted by y,
   the long edge (0-2) spans the whole height and the middle vertex decides
   which side the short edges are on. */
void fill_tri_s(const stri_t *t)
{
    int x0 = t->x0, y0 = t->y0, x1 = t->x1, y1 = t->y1, x2 = t->x2, y2 = t->y2;
    int tmp, mid_left;
    int ya, yb, yc;             /* clipped: top, switch row, bottom */
    fx d02, d01, d12, x02, x01, x12;
    raster_params_t rp;

    if (y1 < y0) { tmp = x0; x0 = x1; x1 = tmp; tmp = y0; y0 = y1; y1 = tmp; }
    if (y2 < y0) { tmp = x0; x0 = x2; x2 = tmp; tmp = y0; y0 = y2; y2 = tmp; }
    if (y2 < y1) { tmp = x1; x1 = x2; x2 = tmp; tmp = y1; y1 = y2; y2 = tmp; }
    if (y2 == y0) return;
    if (y2 <= 0 || y0 >= SCREEN_H) return;
    if (y2 - y0 > 1023) return;

    ya = y0 < 0 ? 0 : y0;
    yc = y2 > SCREEN_H ? SCREEN_H : y2;
    yb = y1 < ya ? ya : (y1 > yc ? yc : y1);

    d02 = edge_slope(x2 - x0, y2 - y0);
    x02 = FX_FROM_INT(x0) + FX_HALF + fxmul(d02, FX_FROM_INT(ya - y0) + FX_HALF);
    if (y1 > y0) {
        d01 = edge_slope(x1 - x0, y1 - y0);
        x01 = FX_FROM_INT(x0) + FX_HALF + fxmul(d01, FX_FROM_INT(ya - y0) + FX_HALF);
        mid_left = d01 < d02;
    } else {
        d01 = 0; x01 = 0;
        mid_left = x1 < x0;
    }
    if (y2 > y1) {
        d12 = edge_slope(x2 - x1, y2 - y1);
        x12 = FX_FROM_INT(x1) + FX_HALF + fxmul(d12, FX_FROM_INT(yb - y1) + FX_HALF);
    } else {
        d12 = 0; x12 = 0;
    }

    rp.row = g_rs->fb + ya * SCREEN_W;
    rp.rows1 = yb - ya;
    rp.rows2 = yc - yb;
    rp.cc = (t->color << 8) | t->color;
    rp.mid_left = mid_left;
    rp.xm = x12; rp.dm = d12;
    if (rp.rows1 == 0) {
        /* flat top (or the upper half is clipped away): start with edge 1-2 */
        if (mid_left) { rp.xl = x12; rp.dl = d12; rp.xr = x02; rp.dr = d02; }
        else { rp.xl = x02; rp.dl = d02; rp.xr = x12; rp.dr = d12; }
        rp.rows1 = rp.rows2; rp.rows2 = 0;
    } else if (mid_left) {
        rp.xl = x01; rp.dl = d01; rp.xr = x02; rp.dr = d02;
    } else {
        rp.xl = x02; rp.dl = d02; rp.xr = x01; rp.dr = d01;
    }
    if (rp.rows1 <= 0) return;
    raster_tri_c(&rp);
}

void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, u8 color)
{
    stri_t t;
    t.x0 = (s16)iclamp(x0, -2048, 2047); t.y0 = (s16)iclamp(y0, -2048, 2047);
    t.x1 = (s16)iclamp(x1, -2048, 2047); t.y1 = (s16)iclamp(y1, -2048, 2047);
    t.x2 = (s16)iclamp(x2, -2048, 2047); t.y2 = (s16)iclamp(y2, -2048, 2047);
    t.color = color;
    fill_tri_s(&t);
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
    render_draw_sorted(rs->tris, rs->order, n);
}

/* raster all queued triangles in the given order (last first) */
void render_draw_sorted(const stri_t *tris, const u16 *order, int n)
{
#ifdef __32X__
    extern void raster_flush_asm(const stri_t *tris, const u16 *order, int n, u8 *fb);
    raster_flush_asm(tris, order, n, g_rs->fb);
#else
    int i;
    for (i = n - 1; i >= 0; i--) fill_tri_s(&tris[order[i]]);
#endif
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
    u32 c32 = ((u32)color << 24) | ((u32)color << 16) | ((u32)color << 8) | color;
    int shift = (x & 3) * 8;             /* misalignment inside the word */
    for (; *s; s++, x += 8) {
        int ch = (u8)*s;
        int r;
        const u8 *g;
        u8 *col;
        if (ch < 32 || ch > 127) ch = '?';
        g = font8[ch - 32];
        if (x < 0 || x + 8 > SCREEN_W) continue;
        col = fb + (x & ~3);
        for (r = 0; r < 8; r++) {
            u8 bits = g[r];
            u8 *row;
            int yy = y + r;
            u32 m0, m1;
            if (yy < 0 || yy >= SCREEN_H || !bits) continue;
            row = col + yy * SCREEN_W;
            m0 = nib_mask[bits >> 4]; m1 = nib_mask[bits & 15];
            if (shift == 0) {
                PUT_MASKED32(row, m0, c32);
                PUT_MASKED32(row + 4, m1, c32);
            } else {
                /* the glyph straddles three words */
                PUT_MASKED32(row, m0 >> shift, c32);
                PUT_MASKED32(row + 4, (m0 << (32 - shift)) | (m1 >> shift), c32);
                if (x + 12 <= SCREEN_W) PUT_MASKED32(row + 8, m1 << (32 - shift), c32);
            }
        }
    }
}

void draw_text_big(int x, int y, const char *s, u8 color)
{
    u8 *fb = g_rs->fb;
    u32 c32 = ((u32)color << 24) | ((u32)color << 16) | ((u32)color << 8) | color;
    int shift = (x & 3) * 8;
    for (; *s; s++, x += 16) {
        int ch = (u8)*s;
        int r;
        const u8 *g;
        u8 *col;
        if (ch < 32 || ch > 127) ch = '?';
        g = font8[ch - 32];
        if (x < 0 || x + 16 > SCREEN_W) continue;
        col = fb + (x & ~3);
        for (r = 0; r < 16; r++) {
            u8 bits = g[r >> 1];
            u8 *row;
            int yy = y + r;
            u32 m[4];
            int k;
            if (yy < 0 || yy >= SCREEN_H || !bits) continue;
            row = col + yy * SCREEN_W;
            /* each glyph bit becomes two pixels */
            for (k = 0; k < 4; k++) {
                int b = (bits >> (6 - k * 2)) & 3;
                m[k] = (b & 2 ? 0xffff0000u : 0) | (b & 1 ? 0x0000ffffu : 0);
            }
            if (shift == 0) {
                for (k = 0; k < 4; k++) PUT_MASKED32(row + k * 4, m[k], c32);
            } else {
                u32 prev = 0;
                for (k = 0; k < 4; k++) {
                    PUT_MASKED32(row + k * 4, (prev << (32 - shift)) | (m[k] >> shift), c32);
                    prev = m[k];
                }
                if (x + 20 <= SCREEN_W) PUT_MASKED32(row + 16, prev << (32 - shift), c32);
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
