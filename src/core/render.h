/*
 * Tux Racer 32X - software 3D renderer (flat shaded, 8bpp, 320x224).
 *
 * The core never touches hardware: it draws into an 8bpp buffer that the
 * platform layer provides (the real 32X framebuffer or a host array).
 */
#ifndef RENDER_H
#define RENDER_H

#include "fixed.h"

#define SCREEN_W 320
#define SCREEN_H 224
#define FOCAL FX(194.0)       /* 60 deg vertical FOV (Tux Racer default) at 224 lines: 112/tan(30) */
#define NEAR_Z FX(0.25)

/* camera */
typedef struct {
    vec3 pos;
    mat34 view;            /* world -> camera (rows are the camera basis) */
    fx far_z;
} camera_t;

extern camera_t g_cam;

/* set camera from view position, direction and up (view.c output) */
void render_set_camera(const vec3 *pos, const vec3 *dir, const vec3 *up);

/* transform world point to camera space */
static inline void cam_transform(vec3 *o, const vec3 *p)
{
    vec3 d;
    d.x = p->x - g_cam.pos.x; d.y = p->y - g_cam.pos.y; d.z = p->z - g_cam.pos.z;
    m34_apply_vec(o, &g_cam.view, &d);
}

/* --- triangle queue (painter's sort) --- */
#define MAX_TRIS 1500

typedef struct {
    s16 x0, y0, x1, y1, x2, y2;   /* screen coords (integer pixels) */
    u8 color;
    u8 flags;
    u16 key;                      /* depth sort key */
} stri_t;

typedef struct {
    u8 *fb;                /* 320*224 8bpp, row pitch 320 */
    int num_tris;
    stri_t tris[MAX_TRIS];
    u16 order[MAX_TRIS];
    u16 tmp_order[MAX_TRIS];
} render_state_t;

extern render_state_t *g_rs;   /* points to a static instance owned by platform */

void render_init(render_state_t *rs, u8 *fb);
void render_begin_frame(void);
/* project camera-space point to screen; returns 0 if behind the near plane */
int render_project(const vec3 *c, int *sx, int *sy);
/* same, table driven (no divide); z must be >= NEAR_Z */
static inline void project_fast(fx x, fx y, fx z, int *sx, int *sy)
{
    /* proj_tab is indexed by z in 1/32 m; it is used from 8 m (index 256,
       error < 0.4 %) to 128 m.  Closer points get the exact divide: the
       quantisation would move near edges by whole pixels. */
    unsigned k = (unsigned)z >> 11;
    fx inv = (k >= 256 && k < 4096) ? proj_tab[k] : fxdiv(FOCAL, z);
    *sx = (SCREEN_W / 2) + fxmul_hi(x, inv);
    *sy = (SCREEN_H / 2) - fxmul_hi(y, inv);
}
/* depth sort key from a camera-space z (12 bits, quasi-logarithmic:
   1/128 m steps below 8 m, 1/16 m steps 8..200 m) */
static inline unsigned depth_key(fx depth)
{
    unsigned key;
    /* shifts by 9 / 12 are libgcc calls on the SH-2 (no barrel shifter):
       use the 8+1 / 8+2+2 forms which are single instructions */
    if (depth < FX(8.0)) {
        if (depth <= 0) return 0;
        key = (unsigned)depth >> 8;
        key >>= 1;
    } else {
        key = (unsigned)(depth - FX(8.0)) >> 8;
        key >>= 2; key >>= 2;
        key += 1024;
    }
    return key > 4095 ? 4095 : key;
}
/* queue a screen-space triangle whose coordinates are already inside
   +-2048 (projected from in-front-of-near-plane points) with a precomputed
   key; returns 0 when the queue is full.  No clipping, no culling. */
static inline int push_tri_fast(int x0, int y0, int x1, int y1, int x2, int y2, u8 color, unsigned key)
{
    render_state_t *rs = g_rs;
    stri_t *t;
    if (rs->num_tris >= MAX_TRIS) return 0;
    t = &rs->tris[rs->num_tris++];
    t->x0 = (s16)x0; t->y0 = (s16)y0; t->x1 = (s16)x1; t->y1 = (s16)y1; t->x2 = (s16)x2; t->y2 = (s16)y2;
    t->color = color; t->flags = 0; t->key = (u16)key;
    return 1;
}
/* queue a screen-space triangle with a camera-space depth */
void render_push_tri(int x0, int y0, int x1, int y1, int x2, int y2, u8 color, fx depth);
/* transform, clip (near plane), backface-cull and queue a world-space triangle */
void render_tri_world(const vec3 *a, const vec3 *b, const vec3 *c, u8 color, int cull);
/* same for camera-space points (already transformed) */
void render_tri_cam(const vec3 *a, const vec3 *b, const vec3 *c, u8 color, int cull);
/* sort and rasterise everything queued */
void render_flush(void);

/* --- immediate 2D primitives (for sky/HUD) --- */
void fill_rect(int x, int y, int w, int h, u8 color);
void fill_tri(int x0, int y0, int x1, int y1, int x2, int y2, u8 color);
void fill_tri_s(const stri_t *t);   /* same, coordinates already clamped to +-2048 */

/* scanline filler parameter block (shared with the SH-2 assembly version
   in src/platform/32x/raster.s - keep the layout in sync) */
typedef struct {
    u8 *row;           /* 0  first scanline */
    int rows1;         /* 4  scanlines before the edge switch */
    int rows2;         /* 8  scanlines after it */
    fx xl, dl;         /* 12 16 left edge (16.16 x at scanline centre, step) */
    fx xr, dr;         /* 20 24 right edge */
    fx xm, dm;         /* 28 32 replacement edge after the switch */
    int mid_left;      /* 36 1: replace the left edge, 0: the right */
    int cc;            /* 40 colour in both bytes */
} raster_params_t;
void raster_tri_c(raster_params_t *rp);
void render_draw_sorted(const stri_t *tris, const u16 *order, int n);
void draw_hline(int x0, int x1, int y, u8 color);
void draw_text(int x, int y, const char *s, u8 color);       /* 8x8 font */
void draw_text_big(int x, int y, const char *s, u8 color);   /* 16x16 (2x) */
int text_width(const char *s);
void draw_number(int x, int y, int n, int digits, u8 color);

/* lighting: returns palette index for (base colour ramp, normal) */
/* palette layout is defined in palette.h */

#endif
