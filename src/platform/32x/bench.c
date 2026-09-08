/* Tux Racer 32X - micro benchmarks (built with -DBENCH; results shown on
   screen and in the telemetry row read by /tmp/telem.py). */
#ifdef BENCH
#include "mars.h"
#include "../../core/game.h"
#include "../../core/render.h"
#include "../../core/palette.h"
#include "../../core/course.h"
#include "../../core/scene.h"
#include "../../core/tuxmodel.h"

static inline unsigned vbl(void) { return MARS_SYS_COMM10; }
unsigned g_bench[16];
volatile fx sink;

static void wait_vbl_edge(void)
{
    unsigned v = vbl();
    while (vbl() == v) ;
}

#ifdef RASTER_DEBUG
static void flip_dbg(void)
{
    volatile u16 *fbctl = (volatile u16 *)0x2000410A;
    u16 cur = *fbctl & 1;
    /* draw into the current back buffer then flip: we drew into the buffer
       we were given, so just flip twice per loop to show both */
    *fbctl = cur ^ 1;
    while ((*fbctl & 1) != (cur ^ 1)) ;
    *fbctl = cur;
    while ((*fbctl & 1) != cur) ;
}
#endif
void run_bench(void)
{
    int i, k;
    unsigned t0;
    vec3 a = { FX(1.5), FX(2.5), FX(3.5) }, c;
    vec3 dir = { 0, FX(-0.3), -FX_ONE }, up = { 0, FX_ONE, 0 };
    vec3 pos = { FX(45.0), FX(10.0), FX(-20.0) };
    course_load(0);
    render_set_camera(&pos, &dir, &up);
    /* make sure the 68k vblank counter is running */
    wait_vbl_edge(); wait_vbl_edge();

    /* 15: assembly vs C rasteriser cross-check.  Pseudo-random triangles
       (including off-screen and degenerate ones) are drawn by both into
       the two framebuffer halves; mismatching 16-bit words are counted. */
    {
        extern void raster_flush_asm(const stri_t *tris, const u16 *order, int n, u8 *fb);
        static stri_t tri[64];
        static u16 ord[64];
        unsigned seed = 12345, bad = 0;
        int n, round;
        volatile u16 *fb16 = (volatile u16 *)MARS_FB_PIXELS;
        for (round = 0; round < 40; round++) {
            int x, y;
            for (n = 0; n < 64; n++) {
                stri_t *t = &tri[n];
                int span = round < 20 ? 24 : 400;      /* small, then big/clipped */
                seed = seed * 1103515245u + 12345u;
                t->x0 = (s16)((int)((seed >> 4) % span) - span / 8);
                t->y0 = (s16)((int)((seed >> 12) % span) - span / 8);
                seed = seed * 1103515245u + 12345u;
                t->x1 = (s16)((int)((seed >> 4) % span) - span / 8);
                t->y1 = (s16)((int)((seed >> 12) % span) - span / 8);
                seed = seed * 1103515245u + 12345u;
                t->x2 = (s16)((int)((seed >> 4) % span) - span / 8);
                t->y2 = (s16)((int)((seed >> 12) % span) - span / 8);
                if (round & 1) { t->y1 = t->y0; }        /* flat tops */
                if ((round & 3) == 2) { t->y2 = t->y1; } /* flat bottoms */
                if (t->y0 > 100) t->y0 = (s16)(t->y0 - 100);
                if (t->y1 > 100) t->y1 = (s16)(t->y1 - 100);
                if (t->y2 > 100) t->y2 = (s16)(t->y2 - 100);
                t->color = (u8)(16 + (seed >> 20) % 200);
                t->flags = 0; t->key = 0;
                ord[n] = (u16)n;
            }
            /* both rasterisers clip against the full 224-row screen, so
               draw with the C code, stash the frame in the other
               framebuffer's memory (the FB is double buffered: the back
               buffer image is at +0x20000 of the *other* buffer, but we
               simply use a static copy in SDRAM), then draw with the asm
               and compare whole frames */
            {
                static u16 copy[224 * SCREEN_W / 2];
                render_state_t *rs = g_rs;
                for (y = 0; y < 224 * SCREEN_W / 2; y++) fb16[y] = 0x2020;
                for (n = 63; n >= 0; n--) fill_tri_s(&tri[n]);
                for (y = 0; y < 224 * SCREEN_W / 2; y++) copy[y] = fb16[y];
                for (y = 0; y < 224 * SCREEN_W / 2; y++) fb16[y] = 0x2020;
                raster_flush_asm(tri, ord, 64, rs->fb);
                for (y = 0; y < 224 * SCREEN_W / 2; y++)
                    if (fb16[y] != copy[y]) bad++;
            }
        }
        g_bench[15] = bad;
#ifdef RASTER_DEBUG
        /* leave a visual comparison on screen: C in the top half, asm in
           the bottom half, for the first small round */
        {
            int x, y;
            static stri_t dbg[4] = {
                { 20, 10, 60, 30, 30, 50, 40, 0, 0 },
                { 100, 10, 140, 10, 120, 50, 60, 0, 0 },
                { 200, 10, 240, 50, 200, 50, 80, 0, 0 },
                { 270, 20, 310, 30, 250, 60, 100, 0, 0 },
            };
            static u16 dord[4] = { 0, 1, 2, 3 };
            render_state_t *rs = g_rs;
            for (y = 0; y < 224 * SCREEN_W / 2; y++) fb16[y] = 0x2020;
            for (n = 3; n >= 0; n--) fill_tri_s(&dbg[n]);
            raster_flush_asm(dbg, dord, 4, rs->fb + 112 * SCREEN_W);
            for (;;) { flip_dbg(); }
        }
#endif
    }

    /* calibration: 6..14 replaced below when CALIB / CALIB2 is set */
#ifdef CALIB2
    {
        extern void raster_tri_asm(raster_params_t *rp);
        static stri_t t4 = { 100, 100, 104, 102, 101, 104, 3, 0, 0 };
        static stri_t t20 = { 100, 100, 120, 110, 105, 120, 3, 0, 0 };
        static stri_t tdeg = { 100, 100, 104, 100, 101, 100, 3, 0, 0 };
        static stri_t t1 = { 100, 100, 104, 101, 101, 101, 3, 0, 0 };
        raster_params_t rp;
        /* 6: 10k fill_tri_s 4x4 */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) fill_tri_s(&t4);
        g_bench[6] = vbl() - t0;
        /* 7: 10k fill_tri_s 20x20 */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) fill_tri_s(&t20);
        g_bench[7] = vbl() - t0;
        /* 8: 10k fill_tri_s degenerate (returns after sort) */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) fill_tri_s(&tdeg);
        g_bench[8] = vbl() - t0;
        /* 9: 10k fill_tri_s 1 row */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) fill_tri_s(&t1);
        g_bench[9] = vbl() - t0;
        /* 10: 10k raster_tri_asm 4 rows x 4 px */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) {
            rp.row = (u8 *)MARS_FB_PIXELS + 100 * SCREEN_W; rp.rows1 = 2; rp.rows2 = 2;
            rp.xl = 100 << 16; rp.dl = 0; rp.xr = 104 << 16; rp.dr = 0; rp.xm = 100 << 16; rp.dm = 0; rp.mid_left = 1; rp.cc = 0x0303;
            raster_tri_asm(&rp);
        }
        g_bench[10] = vbl() - t0;
        /* 11: 10k raster_tri_asm 20 rows x 20 px */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) {
            rp.row = (u8 *)MARS_FB_PIXELS + 100 * SCREEN_W; rp.rows1 = 10; rp.rows2 = 10;
            rp.xl = 100 << 16; rp.dl = 0; rp.xr = 120 << 16; rp.dr = 0; rp.xm = 100 << 16; rp.dm = 0; rp.mid_left = 1; rp.cc = 0x0303;
            raster_tri_asm(&rp);
        }
        g_bench[11] = vbl() - t0;
        /* 12: 10k raster_tri_c 20 rows x 20 px */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) {
            rp.row = (u8 *)MARS_FB_PIXELS + 100 * SCREEN_W; rp.rows1 = 10; rp.rows2 = 10;
            rp.xl = 100 << 16; rp.dl = 0; rp.xr = 120 << 16; rp.dr = 0; rp.xm = 100 << 16; rp.dm = 0; rp.mid_left = 1; rp.cc = 0x0303;
            raster_tri_c(&rp);
        }
        g_bench[12] = vbl() - t0;
        /* 13: 10k empty loop iterations with a call to a trivial function */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 10000; i++) render_begin_frame();
        g_bench[13] = vbl() - t0;
        /* 14: 100 x radix sort of 500 (no draw: all degenerate) - the sort part only */
        render_begin_frame();
        for (i = 0; i < 500; i++) render_push_tri(0, 0, 0, 0, 0, 0, 0, ((i * 7919) & 0xffff) + 1);
        wait_vbl_edge(); t0 = vbl();
        for (k = 0; k < 100; k++) { g_rs->num_tris = 500; render_flush(); }
        g_bench[14] = vbl() - t0;
        return;
    }
#endif
#ifdef CALIB
    {
        volatile u32 *fbw = (volatile u32 *)MARS_FB_PIXELS;
        static volatile u32 sdram_buf[64];
        volatile u32 *sw = sdram_buf;
        u32 acc = 1;
        /* 6: 100k framebuffer 32-bit writes */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 100000; i += 4) { fbw[0] = i; fbw[1] = i; fbw[2] = i; fbw[3] = i; }
        g_bench[6] = vbl() - t0;
        /* 7: 100k SDRAM 32-bit writes */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 100000; i += 4) { sw[0] = i; sw[1] = i; sw[2] = i; sw[3] = i; }
        g_bench[7] = vbl() - t0;
        /* 8: 100k SDRAM 32-bit reads */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 100000; i += 4) { acc += sw[0]; acc += sw[1]; acc += sw[2]; acc += sw[3]; }
        g_bench[8] = vbl() - t0;
        /* 9: 100k ROM reads (table) */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 100000; i += 4) { acc += fx_sintab[(i) & 4095]; acc += fx_sintab[(i + 1) & 4095]; acc += fx_sintab[(i + 2) & 4095]; acc += fx_sintab[(i + 3) & 4095]; }
        g_bench[9] = vbl() - t0;
        /* 10: 400k ALU ops (empty-ish loop) */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 100000; i += 4) { acc = acc * 3 + 1; acc ^= i; acc = acc * 3 + 1; acc ^= i; }
        g_bench[10] = vbl() - t0;
        sink = acc;
        /* 11: 100k function calls (m34_apply_vec) */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 100000; i++) { m34_apply_vec(&c, &g_cam.view, &a); a.x = c.x; }
        g_bench[11] = vbl() - t0;
        /* 12: 100k fxdiv */
        wait_vbl_edge(); t0 = vbl();
        for (i = 0; i < 100000; i++) sink = fxdiv(FX(194.0), FX(3.0) + i);
        g_bench[12] = vbl() - t0;
        /* 13: 100k u16 SDRAM read-modify-write (count[] style) */
        {
            static volatile u16 cnt[256];
            wait_vbl_edge(); t0 = vbl();
            for (i = 0; i < 100000; i++) cnt[i & 255]++;
            g_bench[13] = vbl() - t0;
        }
        /* 14: 100k fb 16-bit writes */
        {
            volatile u16 *fbh = (volatile u16 *)MARS_FB_PIXELS;
            wait_vbl_edge(); t0 = vbl();
            for (i = 0; i < 100000; i += 4) { fbh[0] = (u16)i; fbh[1] = (u16)i; fbh[2] = (u16)i; fbh[3] = (u16)i; }
            g_bench[14] = vbl() - t0;
        }
        return;
    }
#endif

    /* 0: 10k fill_tri 20x20 px */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 10000; i++) fill_tri(100, 100, 120, 110, 105, 120, (u8)i);
    g_bench[0] = vbl() - t0;
    /* 1: 10k fill_tri 4x4 px */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 10000; i++) fill_tri(100, 100, 104, 102, 101, 104, (u8)i);
    g_bench[1] = vbl() - t0;
    /* 2: 1k fill_tri 60x60 px */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 1000; i++) fill_tri(100, 60, 160, 90, 110, 120, (u8)i);
    g_bench[2] = vbl() - t0;
    /* 3: 10k fill_tri 8x8 px (typical far terrain) */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 10000; i++) fill_tri(100, 100, 108, 103, 102, 108, (u8)i);
    g_bench[3] = vbl() - t0;
    /* 4: 1000 x fill_row of 60 vertices (terrain vertex transform+project) */
    {
        extern void scene_bench_fill_row(int n, int count);
        wait_vbl_edge(); t0 = vbl();
        scene_bench_fill_row(1000, 60);
        g_bench[4] = vbl() - t0;
    }
    /* 5: 100 x radix sort of 500 tris (no raster: degenerate tris) */
    render_begin_frame();
    for (i = 0; i < 500; i++) render_push_tri(0, 0, 0, 0, 0, 0, 0, ((i * 7919) & 0xffff) + 1);
    wait_vbl_edge(); t0 = vbl();
    for (k = 0; k < 100; k++) { g_rs->num_tris = 500; render_flush(); }
    g_bench[5] = vbl() - t0;
    /* 6: 100k cam_transform */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 100000; i++) { a.x += 1; cam_transform(&c, &a); sink = c.z; }
    g_bench[6] = vbl() - t0;
    /* 7: 100k render_project */
    wait_vbl_edge(); t0 = vbl();
    c.x = FX(1.0); c.y = FX(2.0); c.z = FX(5.0);
    for (i = 0; i < 100000; i++) { int sx, sy; c.z += 1; render_project(&c, &sx, &sy); sink = sx; }
    g_bench[7] = vbl() - t0;
    /* 8: 10k render_tri_cam (visible, no clip) */
    wait_vbl_edge(); t0 = vbl();
    {
        vec3 p0 = { FX(-1.0), FX(-1.0), FX(6.0) }, p1 = { FX(1.0), FX(-1.0), FX(6.0) }, p2 = { 0, FX(1.0), FX(6.0) };
        for (i = 0; i < 10000; i++) { render_begin_frame(); render_tri_cam(&p0, &p2, &p1, 16, 1); }
    }
    g_bench[8] = vbl() - t0;
    /* 9: 10k course_find_y */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 10000; i++) sink = course_find_y(FX(40.0) + i * 100, FX(-20.0) - i * 100);
    g_bench[9] = vbl() - t0;
    /* 10: 20 x scene_draw_terrain (geometry only) */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 20; i++) { render_begin_frame(); scene_draw_terrain(pos.z, pos.x); }
    g_bench[10] = vbl() - t0;
    g_bench[11] = g_rs->num_tris;
    /* 12: 20 x flush of the terrain queue (sort + raster) */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 20; i++) { render_flush(); }
    g_bench[12] = vbl() - t0;
    /* 13: 20 x objects + tux geometry */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 20; i++) {
        mat34 root; tux_pose_t pose; vec3 lf = {0, 0, 0};
        render_begin_frame();
        scene_draw_objects(pos.z);
        m34_identity(&root); root.t = pos; root.t.z -= FX(4.0); root.t.y -= FX(2.0);
        tux_pose_racing(&pose, 0, 0, 0, FX(10), &lf, 0);
        scene_draw_tux(&root, &pose);
    }
    g_bench[13] = vbl() - t0;
    /* 14: 20 x flush of the object queue */
    wait_vbl_edge(); t0 = vbl();
    for (i = 0; i < 20; i++) { render_flush(); }
    g_bench[14] = vbl() - t0;
}
#endif
