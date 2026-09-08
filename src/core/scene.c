/*
 * Tux Racer 32X - scene drawing.
 *
 * Terrain: the heightmap grid around the camera is drawn as real
 * triangles (the same alternating-diagonal mesh Tux Racer uses, see
 * course_render.c) with per-triangle flat shading derived from the
 * vertex normals.  Rows far from the camera are drawn at half
 * resolution to stay within the triangle budget.
 *
 * Trees: Tux Racer draws them as textured billboards; here they are real
 * 3D objects - a cone (evergreen) or a tapered trunk with a sparse crown
 * (barren), sized with the course's per-tree height/diameter.  The
 * collision volume is the original tree polyhedron.
 *
 * Tux: the tux.tcl ellipsoid model evaluated with the joint hierarchy,
 * each ellipsoid tessellated as a low-poly UV sphere.
 */
#include "scene.h"
#include "render.h"
#include "palette.h"
#include "course.h"

int g_scene_lod = 0;
int g_scene_tri_budget = 900;

/* ---------------------------------------------------------------- sky */

void scene_draw_sky(void)
{
    /* Simple gradient: 8 bands over the full screen; the terrain covers
       the lower part.  Painted with 16-bit writes. */
    int y, band = 0, next = SCREEN_H / 8;
    u32 *p = (u32 *)g_rs->fb;
    for (y = 0; y < SCREEN_H; y++) {
        u32 c;
        u32 cc;
        int n;
        if (y == next) { band++; next = ((band + 1) * SCREEN_H) / 8; }
        c = (u32)(PAL_SKY + band);
        cc = (c << 24) | (c << 16) | (c << 8) | c;
        n = SCREEN_W / 4;
        while (n >= 8) {
            p[0] = cc; p[1] = cc; p[2] = cc; p[3] = cc; p[4] = cc; p[5] = cc; p[6] = cc; p[7] = cc;
            p += 8; n -= 8;
        }
    }
}

/* ------------------------------------------------------------- terrain */

static inline u8 terrain_color(int terr, int level, fx depth)
{
    int base;
    /* fog: Tux Racer uses linear white fog to forward_clip_distance (75 m);
       fade snow shading toward white at distance. */
    if (depth > FX(45.0)) {
        int fl = (depth - FX(45.0)) >> 19;   /* /8 m */
        level += fl * 2;
        if (level > 15) level = 15;
    }
    switch (terr) {
    case TERRAIN_ICE: base = PAL_ICE; break;
    case TERRAIN_ROCK: base = PAL_ROCK; break;
    default: base = PAL_SNOW; break;
    }
    return (u8)(base + level);
}

/* Row cache: two consecutive terrain rows in camera space, projected.
   The grid is regular, so world -> camera is incremental: stepping one
   column adds the constant vector (view column 0) * xcd_step, and the
   elevation contributes (view column 1) * y.  That leaves 3 multiplies per
   vertex instead of 9 plus a subtraction and function call. */
#define ROWBUF 128
typedef struct {
    fx cx[ROWBUF], cy[ROWBUF], cz[ROWBUF];   /* camera space */
    s16 sx[ROWBUF], sy[ROWBUF];              /* screen (valid when ok) */
    u8 ok[ROWBUF];                           /* 1: in front of the near plane */
    u8 lvl[ROWBUF];                          /* baked shade level */
    u8 terr[ROWBUF];
} rowcache_t;

static rowcache_t rows[2];

/* 2-bit terrain samples, 4 per byte: unpack_terr[byte][sample] (the SH-2
   has no variable shift, so a shift by (idx & 3) * 2 costs a libgcc call) */
static u8 unpack_terr[256][4];
static int unpack_terr_ready;

static void fill_row(rowcache_t *r, int y, int x0, int count, int step)
{
    const course_def_t *d = g_course.def;
    const mat34 *V = &g_cam.view;
    int rowbase = d->nx * y;
    const u8 *elev = d->elev + rowbase + x0;
    const u8 *shade = d->shade + rowbase + x0;
    const u8 *terr = d->terrain;
    int tidx = rowbase + x0;
    int base_height = d->base_height;
    fx *cxp = r->cx, *cyp = r->cy, *czp = r->cz;
    s16 *sxp = r->sx, *syp = r->sy;
    u8 *okp = r->ok, *lvlp = r->lvl, *terrp = r->terr;
    /* camera-space position of the vertex (x0, y) at elevation pixel =
       base_height, and the increments per grid column / per pixel */
    fx wx = course_xcd(x0) - g_cam.pos.x;
    fx wz = course_zcd(y) - g_cam.pos.z;
    fx y_off = -fxmul(FX_FROM_INT(y), g_course.dz_per_row) - g_cam.pos.y;
    fx ox = fxmul(V->m[0][0], wx) + fxmul(V->m[0][1], y_off) + fxmul(V->m[0][2], wz);
    fx oy = fxmul(V->m[1][0], wx) + fxmul(V->m[1][1], y_off) + fxmul(V->m[1][2], wz);
    fx oz = fxmul(V->m[2][0], wx) + fxmul(V->m[2][1], y_off) + fxmul(V->m[2][2], wz);
    fx sxs = fxmul(V->m[0][0], g_course.xcd_step) * step;
    fx sys = fxmul(V->m[1][0], g_course.xcd_step) * step;
    fx szs = fxmul(V->m[2][0], g_course.xcd_step) * step;
    fx ex = fxmul(V->m[0][1], g_course.elev_scale_255);
    fx ey = fxmul(V->m[1][1], g_course.elev_scale_255);
    fx ez = fxmul(V->m[2][1], g_course.elev_scale_255);
    int i;
    if (!unpack_terr_ready) {
        int b, k;
        for (b = 0; b < 256; b++) for (k = 0; k < 4; k++) unpack_terr[b][k] = (u8)((b >> (k * 2)) & 3);
        unpack_terr_ready = 1;
    }
    if (count > ROWBUF) count = ROWBUF;
    for (i = 0; i < count; i++) {
        int pix = (int)*elev - base_height;
        fx cx = ox + ex * pix;
        fx cy = oy + ey * pix;
        fx cz = oz + ez * pix;
        *cxp++ = cx; *cyp++ = cy; *czp++ = cz;
        if (cz >= NEAR_Z) {
            int sx, sy;
            project_fast(cx, cy, cz, &sx, &sy);
            /* keep the s16 fields (and the rasteriser's slope table) safe */
            if (sx < -2048) sx = -2048; else if (sx > 2047) sx = 2047;
            if (sy < -2048) sy = -2048; else if (sy > 2047) sy = 2047;
            *sxp = (s16)sx; *syp = (s16)sy;
            *okp = 1;
        } else {
            *okp = 0;
        }
        sxp++; syp++; okp++;
        *lvlp++ = *shade;
        *terrp++ = unpack_terr[terr[tidx >> 2]][tidx & 3];
        elev += step; shade += step; tidx += step;
        ox += sxs; oy += sys; oz += szs;
    }
}

#ifdef BENCH
/* micro-benchmark hooks (bench.c) */
void scene_bench_fill_row(int n, int count)
{
    int i;
    for (i = 0; i < n; i++) fill_row(&rows[i & 1], 10 + (i & 7), 0, count, 1);
}
#endif

/* distance zones: (max distance m, x step, y step in metres of grid) */
typedef struct { fx max_dist; int xstep_m, ystep_m; } terrain_zone_t;
static const terrain_zone_t zones[4] = {
    { FX(12.0), 1, 1 },
    { FX(28.0), 2, 2 },
    { FX(50.0), 4, 4 },
    { FX(80.0), 8, 8 },
};

/* Emit one terrain triangle from the row caches.  All three vertices
   projected: push directly (screen-space frustum reject only).  Otherwise
   fall back to the general near-plane clipping path. */
static inline void terrain_tri(const rowcache_t *ra, int ia, const rowcache_t *rb, int ib,
                               const rowcache_t *rc, int ic, u8 col, fx far)
{
    fx az = ra->cz[ia], bz = rb->cz[ib], cz = rc->cz[ic];
    if (az > far && bz > far && cz > far) return;
    if (ra->ok[ia] & rb->ok[ib] & rc->ok[ic]) {
        int x0 = ra->sx[ia], y0 = ra->sy[ia], x1 = rb->sx[ib], y1 = rb->sy[ib], x2 = rc->sx[ic], y2 = rc->sy[ic];
        fx depth;
        if ((x0 < 0 && x1 < 0 && x2 < 0) || (x0 >= SCREEN_W && x1 >= SCREEN_W && x2 >= SCREEN_W)) return;
        if ((y0 < 0 && y1 < 0 && y2 < 0) || (y0 >= SCREEN_H && y1 >= SCREEN_H && y2 >= SCREEN_H)) return;
        depth = (az + bz + cz) / 3;             /* same key as render_tri_cam */
        push_tri_fast(x0, y0, x1, y1, x2, y2, col, depth_key(depth));
    } else {
        vec3 a, b, c;
        a.x = ra->cx[ia]; a.y = ra->cy[ia]; a.z = ra->cz[ia];
        b.x = rb->cx[ib]; b.y = rb->cy[ib]; b.z = rb->cz[ib];
        c.x = rc->cx[ic]; c.y = rc->cy[ic]; c.z = rc->cz[ic];
        render_tri_cam(&a, &b, &c, col, 0);
    }
}

static void draw_terrain_zone(int y_from, int y_to, int ystep, int x0, int x1, int xstep, fx far)
{
    int cur = 0, y, count, i;
    if (y_to <= y_from) return;
    x1 = x0 + ((x1 - x0) / xstep) * xstep;
    count = (x1 - x0) / xstep + 1;
    if (count < 2) return;
    if (count > ROWBUF) count = ROWBUF;
    fill_row(&rows[cur], y_from, x0, count, xstep);
    for (y = y_from; y < y_to; y += ystep) {
        int nxt = cur ^ 1;
        int y2 = y + ystep;
        rowcache_t *ra, *rb;
        int diag = ((x0 + y) & 1) == 0 || xstep > 1;   /* diagonal orientation of the first cell */
        if (y2 > y_to) y2 = y_to;
        fill_row(&rows[nxt], y2, x0, count, xstep);
        ra = &rows[cur]; rb = &rows[nxt];
        /* a = ra[i], b = ra[i+1], c = rb[i], d = rb[i+1] */
        for (i = 0; i < count - 1; i++, diag ^= (xstep == 1)) {
            fx az = ra->cz[i], bz = ra->cz[i + 1], cz = rb->cz[i], dz = rb->cz[i + 1];
            fx depth, k83;
            int lvl1, lvl2, t1, t2;
            u8 col1, col2;
            if (az < NEAR_Z && bz < NEAR_Z && cz < NEAR_Z && dz < NEAR_Z) continue;
            if (az > far && bz > far && cz > far && dz > far) continue;
            /* cheap frustum reject on the cell (all four vertices outside
               one side); in front of the near plane the screen test in
               terrain_tri is exact, this catches cells partly behind us */
            {
                fx ax = ra->cx[i], bx = ra->cx[i + 1], ccx = rb->cx[i], dx = rb->cx[i + 1];
                fx ka, kb, kc, kd;
                k83 = FX(0.83);
                ka = fxmul(az, k83); kb = fxmul(bz, k83); kc = fxmul(cz, k83); kd = fxmul(dz, k83);
                if (ax > ka && bx > kb && ccx > kc && dx > kd) continue;
                if (ax < -ka && bx < -kb && ccx < -kc && dx < -kd) continue;
            }
            depth = (az + dz) >> 1;
            if (diag) {
                lvl1 = (ra->lvl[i] + rb->lvl[i] + rb->lvl[i + 1]) / 3;
                lvl2 = (ra->lvl[i] + rb->lvl[i + 1] + ra->lvl[i + 1]) / 3;
                t1 = rb->terr[i]; t2 = ra->terr[i + 1];
                if (ra->terr[i] == rb->terr[i]) t1 = ra->terr[i];
                if (ra->terr[i] == rb->terr[i + 1]) t2 = ra->terr[i];
                col1 = terrain_color(t1, lvl1, depth);
                col2 = terrain_color(t2, lvl2, depth);
                terrain_tri(ra, i, rb, i, rb, i + 1, col1, far);
                terrain_tri(ra, i, rb, i + 1, ra, i + 1, col2, far);
            } else {
                lvl1 = (ra->lvl[i] + rb->lvl[i] + ra->lvl[i + 1]) / 3;
                lvl2 = (ra->lvl[i + 1] + rb->lvl[i] + rb->lvl[i + 1]) / 3;
                t1 = ra->terr[i]; t2 = rb->terr[i + 1];
                if (rb->terr[i] == ra->terr[i + 1]) { t1 = rb->terr[i]; t2 = rb->terr[i]; }
                col1 = terrain_color(t1, lvl1, depth);
                col2 = terrain_color(t2, lvl2, depth);
                terrain_tri(ra, i, rb, i, ra, i + 1, col1, far);
                terrain_tri(ra, i + 1, rb, i, rb, i + 1, col2, far);
            }
            if (g_rs->num_tris >= MAX_TRIS - 8) return;
        }
        cur = nxt;
    }
}

void scene_draw_terrain(fx cam_z, fx cam_x)
{
    int ny = g_course.ny, nx = g_course.nx;
    fx far = g_cam.far_z;
    int ycam, zi;
    int xc = FX_INT(fxmul(cam_x, g_course.inv_xcd_step));
    /* metres per grid step (rounded to >= 1) */
    int xm = FX_INT(g_course.xcd_step + FX_HALF), zm = FX_INT(g_course.zcd_step + FX_HALF);
    int prev_far_row;
    if (xm < 1) xm = 1;
    if (zm < 1) zm = 1;
    ycam = FX_INT(fxmul(-cam_z, g_course.inv_zcd_step));
    /* zone 0 starts behind the camera (rows uphill of it) */
    prev_far_row = ycam - (int)FX_INT(fxmul(FX(8.0), g_course.inv_zcd_step)) - 1;
    if (prev_far_row < 0) prev_far_row = 0;
    for (zi = 0; zi < 4; zi++) {
        const terrain_zone_t *z = &zones[zi];
        int xstep = z->xstep_m / xm, ystep = z->ystep_m / zm;
        int y_to, y_from, x0, x1, half;
        if (xstep < 1) xstep = 1;
        if (ystep < 1) ystep = 1;
        if (g_scene_lod && zi > 0) { xstep *= 2; ystep *= 2; }
        y_to = ycam + (int)FX_INT(fxmul(fminx(z->max_dist, far), g_course.inv_zcd_step)) + 1;
        if (y_to > ny - 1) y_to = ny - 1;
        /* start one coarse step before the previous zone's end so the
           seam is covered by both meshes (no cracks) */
        y_from = prev_far_row - ystep;
        y_from = (y_from / ystep) * ystep;
        if (y_from < 0) y_from = 0;
        /* horizontal window: frustum width at the zone's far distance */
        half = (int)FX_INT(fxmul(fxmul(z->max_dist, FX(0.9)), g_course.inv_xcd_step)) + xstep;
        x0 = ((xc - half) / xstep) * xstep;
        x1 = xc + half;
        if (x0 < 0) x0 = 0;
        if (x1 > nx - 1) x1 = nx - 1;
        draw_terrain_zone(y_from, y_to, ystep, x0, x1, xstep, far);
        prev_far_row = y_to;
        if (y_to >= ny - 1) break;
        if (g_rs->num_tris >= MAX_TRIS - 8) break;
    }
}

/* -------------------------------------------------------------- objects */

/* unit circle (8 points) in 16.16 */
static const fx circ8[8][2] = {
    { FX_ONE, 0 }, { FX(0.7071), FX(0.7071) }, { 0, FX_ONE }, { FX(-0.7071), FX(0.7071) },
    { -FX_ONE, 0 }, { FX(-0.7071), FX(-0.7071) }, { 0, -FX_ONE }, { FX(0.7071), FX(-0.7071) }
};

/* Trees are built from a handful of world-space offsets around the base
   point (ring of 8 around the trunk, the tip straight up).  The camera
   rotation is linear, so those offsets are rotated once per frame
   (scene_draw_objects) and each tree only needs its base transformed:
   vertex = cbase + r * cring[i] + h * cup. */
static vec3 cring[8];          /* camera-space unit circle in the xz plane */
static vec3 cup;               /* camera-space world up */

static inline void tree_vert(vec3 *o, const vec3 *cbase, fx r, int ci, fx h)
{
    o->x = cbase->x + fxmul(r, cring[ci].x) + fxmul(h, cup.x);
    o->y = cbase->y + fxmul(r, cring[ci].y) + fxmul(h, cup.y);
    o->z = cbase->z + fxmul(r, cring[ci].z) + fxmul(h, cup.z);
}

/* triangle from three camera-space points that are known to be in front
   of the near plane: project + push, no clipping; culls back faces when
   cull is set (counter-clockwise on screen = front) */
static inline void tri_cam_front(const vec3 *a, const vec3 *b, const vec3 *c, u8 color, int cull)
{
    int x0, y0, x1, y1, x2, y2;
    fx depth = (a->z + b->z + c->z) / 3;
    project_fast(a->x, a->y, a->z, &x0, &y0);
    project_fast(b->x, b->y, b->z, &x1, &y1);
    project_fast(c->x, c->y, c->z, &x2, &y2);
    if ((x0 < 0 && x1 < 0 && x2 < 0) || (x0 >= SCREEN_W && x1 >= SCREEN_W && x2 >= SCREEN_W)) return;
    if ((y0 < 0 && y1 < 0 && y2 < 0) || (y0 >= SCREEN_H && y1 >= SCREEN_H && y2 >= SCREEN_H)) return;
    if (cull && (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0) >= 0) return;
    render_push_tri(x0, y0, x1, y1, x2, y2, color, depth);
}

static void draw_tree(const vec3 *cbase, fx height, fx diam, int type, fx dist)
{
    int i, n;
    fx r = diam >> 1;
    int sides = dist > FX(30.0) ? 4 : 8;
    int stepi = 8 / sides;
    /* everything in front of the near plane (with margin)?  then the
       cheap path applies; otherwise use the clipping one */
    int front = cbase->z - r - fxmul(height, fabsx(cup.z)) - r > NEAR_Z;

    if (cbase->z < NEAR_Z - r && cbase->z + height < NEAR_Z) return;

    if (type == 2) {
        /* barren tree: thin trunk + a few branch fins */
        fx tr = fxmul(r, FX(0.18));
        vec3 ct0, ct1, ct2, ct3, ctop;
        tree_vert(&ctop, cbase, 0, 0, height);
        for (i = 0; i < 4; i++) {
            tree_vert(&ct0, cbase, tr, i * 2, 0);
            tree_vert(&ct1, cbase, tr, (i * 2 + 2) & 7, 0);
            if (front) tri_cam_front(&ct0, &ct1, &ctop, (u8)(PAL_TRUNK + 6 + i * 2), 0);
            else render_tri_cam(&ct0, &ct1, &ctop, (u8)(PAL_TRUNK + 6 + i * 2), 0);
        }
        /* two crossed branch fins */
        for (i = 0; i < 2; i++) {
            fx h45 = fxmul(height, FX(0.45));
            tree_vert(&ct0, cbase, r, i ? 4 : 6, h45);      /* -bx / -bz side */
            tree_vert(&ct1, cbase, r, i ? 0 : 2, h45);      /* +bx / +bz side */
            tree_vert(&ct2, cbase, 0, 0, fxmul(height, FX(0.9)));
            tree_vert(&ct3, cbase, 0, 0, fxmul(height, FX(0.3)));
            if (front) {
                tri_cam_front(&ct0, &ct2, &ct3, (u8)(PAL_TREE_BARREN + 8 + i * 3), 0);
                tri_cam_front(&ct1, &ct3, &ct2, (u8)(PAL_TREE_BARREN + 6 + i * 3), 0);
            } else {
                render_tri_cam(&ct0, &ct2, &ct3, (u8)(PAL_TREE_BARREN + 8 + i * 3), 0);
                render_tri_cam(&ct1, &ct3, &ct2, (u8)(PAL_TREE_BARREN + 6 + i * 3), 0);
            }
        }
        return;
    }

    /* evergreen (tree1) / shrub (tree3): cone (2 stacked for tall trees) */
    n = (type == 1 && dist < FX(30.0)) ? 2 : 1;
    {
        int layer;
        for (layer = 0; layer < n; layer++) {
            fx ly = layer == 0 ? 0 : fxmul(height, FX(0.35));
            fx lh = n == 1 ? height : fxmul(height, FX(0.65));
            fx lr = layer == 0 ? r : fxmul(r, FX(0.7));
            vec3 ctp, c0, c1;
            tree_vert(&ctp, cbase, 0, 0, ly + lh);
            tree_vert(&c0, cbase, lr, 0, ly);
            for (i = 0; i < 8; i += stepi) {
                int j = (i + stepi) & 7;
                int lvl;
                tree_vert(&c1, cbase, lr, j, ly);
                /* face normal approx: outward radial direction -> shade by light (1,1,0) */
                {
                    fx nx = (circ8[i][0] + circ8[j][0]) >> 1;
                    fx d = fxmul(nx, FX(0.6)) + FX(0.55);     /* radial x + upward tilt */
                    lvl = (d * 15) >> 16;
                    if (lvl < 2) lvl = 2; if (lvl > 15) lvl = 15;
                }
                if (front) tri_cam_front(&c1, &c0, &ctp, (u8)(PAL_TREE_GREEN + lvl), 1);
                else render_tri_cam(&c1, &c0, &ctp, (u8)(PAL_TREE_GREEN + lvl), 1);
                c0 = c1;
            }
        }
    }
    /* trunk for tall trees when close */
    if (type == 1 && dist < FX(20.0)) {
        fx tr = fxmul(r, FX(0.15));
        vec3 ct0, ct1, ct2, ct3;
        tree_vert(&ct0, cbase, tr, 4, -FX(0.3)); tree_vert(&ct1, cbase, tr, 0, -FX(0.3));
        tree_vert(&ct2, cbase, tr, 0, FX(0.4)); tree_vert(&ct3, cbase, tr, 4, FX(0.4));
        if (front) {
            tri_cam_front(&ct0, &ct1, &ct2, PAL_TRUNK + 8, 0);
            tri_cam_front(&ct0, &ct2, &ct3, PAL_TRUNK + 8, 0);
        } else {
            render_tri_cam(&ct0, &ct1, &ct2, PAL_TRUNK + 8, 0);
            render_tri_cam(&ct0, &ct2, &ct3, PAL_TRUNK + 8, 0);
        }
    }
}

static void draw_herring(fx x, fx z, fx above, fx height, int idx)
{
    /* a small fish: two triangles body (diamond) + tail, facing camera-ish
       (rotated slowly so it glints).  Real geometry in 3D. */
    fx y = course_find_y(x, z) + above + (height >> 1);
    fx s = FX(0.45);
    vec3 p[6], c[6];
    fx ang = FX_FROM_INT(((idx * 37) & 255)) * 1 + 0;
    fx sa, ca;
    int i;
    extern int g_frame_counter;
    ang += FX_FROM_INT((g_frame_counter * 3) & 359);
    sa = fx_sin_deg(ang); ca = fx_cos_deg(ang);
    /* body diamond in local x/y plane, rotated about y */
    {
        fx lx[6] = { -s, 0, s, 0, s, fxmul(s, FX(1.7)) };
        fx ly[6] = { 0, fxmul(s, FX(0.45)), 0, -fxmul(s, FX(0.45)), 0, 0 };
        for (i = 0; i < 6; i++) {
            fx wx = fxmul(lx[i], ca), wz = fxmul(lx[i], sa);
            v3_set(&p[i], x + wx, y + ly[i], z + wz);
        }
        /* tail fin */
        v3_set(&p[4], x + fxmul(fxmul(s, FX(1.6)), ca), y + fxmul(s, FX(0.5)), z + fxmul(fxmul(s, FX(1.6)), sa));
        v3_set(&p[5], x + fxmul(fxmul(s, FX(1.6)), ca), y - fxmul(s, FX(0.5)), z + fxmul(fxmul(s, FX(1.6)), sa));
    }
    for (i = 0; i < 6; i++) cam_transform(&c[i], &p[i]);
    render_tri_cam(&c[0], &c[1], &c[2], PAL_HERRING + 12, 0);
    render_tri_cam(&c[0], &c[2], &c[3], PAL_HERRING + 8, 0);
    render_tri_cam(&c[2], &c[4], &c[5], PAL_HERRING + 10, 0);
}

static void draw_flag(fx x, fx z, fx height)
{
    fx y = course_find_y(x, z);
    vec3 p0, p1, p2, p3, c0, c1, c2, c3;
    v3_set(&p0, x, y, z);
    v3_set(&p1, x, y + height, z);
    v3_set(&p2, x + FX(0.6), y + height - FX(0.15), z);
    v3_set(&p3, x + FX(0.6), y + height - FX(0.5), z);
    cam_transform(&c0, &p0); cam_transform(&c1, &p1); cam_transform(&c2, &p2); cam_transform(&c3, &p3);
    /* pole */
    {
        vec3 q0, q1, cq0, cq1;
        v3_set(&q0, x + FX(0.05), y, z); v3_set(&q1, x + FX(0.05), y + height, z);
        cam_transform(&cq0, &q0); cam_transform(&cq1, &q1);
        render_tri_cam(&c0, &cq0, &c1, PAL_UI_GREY, 0);
        render_tri_cam(&cq0, &cq1, &c1, PAL_UI_GREY, 0);
    }
    render_tri_cam(&c1, &c2, &c3, PAL_FLAG + 12, 0);
    render_tri_cam(&c1, &c3, &c2, PAL_FLAG + 12, 0);
}

static void draw_banner(fx x, fx z, fx diam, fx height, int finish)
{
    /* start/finish: two poles and a banner across (Tux Racer: 9 m wide, 6 m tall texture) */
    fx y0 = course_find_y(x - (diam >> 1), z), y1 = course_find_y(x + (diam >> 1), z);
    fx top = fmaxx(y0, y1) + height;
    vec3 p[8], c[8];
    int i;
    fx pw = FX(0.25);
    v3_set(&p[0], x - (diam >> 1) - pw, y0, z);
    v3_set(&p[1], x - (diam >> 1) + pw, y0, z);
    v3_set(&p[2], x - (diam >> 1) + pw, top, z);
    v3_set(&p[3], x - (diam >> 1) - pw, top, z);
    v3_set(&p[4], x + (diam >> 1) - pw, y1, z);
    v3_set(&p[5], x + (diam >> 1) + pw, y1, z);
    v3_set(&p[6], x + (diam >> 1) + pw, top, z);
    v3_set(&p[7], x + (diam >> 1) - pw, top, z);
    for (i = 0; i < 8; i++) cam_transform(&c[i], &p[i]);
    render_tri_cam(&c[0], &c[1], &c[2], PAL_UI_GREY, 0);
    render_tri_cam(&c[0], &c[2], &c[3], PAL_UI_GREY, 0);
    render_tri_cam(&c[4], &c[5], &c[6], PAL_UI_GREY, 0);
    render_tri_cam(&c[4], &c[6], &c[7], PAL_UI_GREY, 0);
    /* banner: checker strips */
    {
        int k, nstrip = 6;
        fx bw = diam;
        fx bh = fxmul(height, FX(0.28));
        for (k = 0; k < nstrip; k++) {
            vec3 q[4], cq[4];
            fx xa = x - (diam >> 1) + fxdiv(fxmul(bw, FX_FROM_INT(k)), FX_FROM_INT(nstrip));
            fx xb = x - (diam >> 1) + fxdiv(fxmul(bw, FX_FROM_INT(k + 1)), FX_FROM_INT(nstrip));
            u8 col = (u8)(finish ? PAL_FINISH + (k & 1) : (k & 1 ? PAL_UI_WHITE : PAL_UI_BLUE));
            v3_set(&q[0], xa, top - bh, z); v3_set(&q[1], xb, top - bh, z);
            v3_set(&q[2], xb, top, z); v3_set(&q[3], xa, top, z);
            for (i = 0; i < 4; i++) cam_transform(&cq[i], &q[i]);
            render_tri_cam(&cq[0], &cq[1], &cq[2], col, 0);
            render_tri_cam(&cq[0], &cq[2], &cq[3], col, 0);
        }
    }
}

void scene_draw_objects(fx cam_z)
{
    const course_def_t *d = g_course.def;
    fx far = g_cam.far_z;
    fx zmin = cam_z - far, zmax = cam_z + FX(12.0);
    int i;
    int tree_budget = g_scene_lod ? 40 : 90;
    /* per-frame: rotate the tree template offsets into camera space */
    for (i = 0; i < 8; i++) {
        vec3 w;
        w.x = circ8[i][0]; w.y = 0; w.z = circ8[i][1];
        m34_apply_vec_i(&cring[i], &g_cam.view, &w);
    }
    {
        vec3 w = { 0, FX_ONE, 0 };
        m34_apply_vec_i(&cup, &g_cam.view, &w);
    }
    /* trees sorted by z descending (start first). Find first tree with z <= zmax. */
    for (i = 0; i < d->num_trees; i++) {
        const tree_def_t *t = &d->trees[i];
        fx tz = POS11_5_TO_FX(t->z);
        fx tx, dist;
        vec3 w, c;
        if (tz > zmax) continue;
        if (tz < zmin) break;
        tx = POS11_5_TO_FX(t->x);
        /* view frustum quick test using camera space */
        w.x = tx; w.y = course_find_y(tx, tz); w.z = tz;
        cam_transform(&c, &w);
        if (c.z < -FX(2.0) || c.z > far) continue;
        if (fabsx(c.x) > c.z + FX(4.0)) continue;   /* outside ~90 deg fov */
        dist = c.z;
        if (dist > FX(55.0) && (i & 1)) continue;   /* thin far trees */
        draw_tree(&c, FX8_8_TO_FX(t->height), FX8_8_TO_FX(t->diam), t->type, dist);
        if (--tree_budget <= 0) break;
        if (g_rs->num_tris >= MAX_TRIS - 40) break;
    }
    for (i = 0; i < d->num_items && i < MAX_ITEMS; i++) {
        const item_def_t *it = &d->items[i];
        fx iz = POS11_5_TO_FX(it->z), ix;
        vec3 w, c;
        if (iz > zmax) continue;
        if (iz < zmin) break;
        if (it->kind == ITEM_FLOAT) continue;
        if (it->collectable && g_course.item_collected[i]) continue;
        ix = POS11_5_TO_FX(it->x);
        w.x = ix; w.y = course_find_y(ix, iz) + FX_ONE; w.z = iz;
        cam_transform(&c, &w);
        if (c.z < -FX(2.0) || c.z > far) continue;
        if (it->kind == ITEM_HERRING) {
            if (fabsx(c.x) > c.z + FX(2.0)) continue;
            draw_herring(ix, iz, FX8_8_TO_FX(it->above), FX8_8_TO_FX(it->height), i);
        } else if (it->kind == ITEM_FLAG) {
            if (fabsx(c.x) > c.z + FX(2.0)) continue;
            draw_flag(ix, iz, FX8_8_TO_FX(it->height));
        } else if (it->kind == ITEM_FINISH || it->kind == ITEM_START) {
            draw_banner(ix, iz, FX8_8_TO_FX(it->diam), FX8_8_TO_FX(it->height), it->kind == ITEM_FINISH);
        }
        if (g_rs->num_tris >= MAX_TRIS - 40) break;
    }
}

/* ------------------------------------------------------------------ Tux */

#define TUX_MAX_FRAME_VERTS 128
static vec3 tux_cam[TUX_MAX_FRAME_VERTS];
static int tux_sx[TUX_MAX_FRAME_VERTS], tux_sy[TUX_MAX_FRAME_VERTS];
static u8 tux_ok[TUX_MAX_FRAME_VERTS];

static u8 tux_material_base(int m)
{
    switch (m) {
    case TUX_MAT_WHITE: return PAL_TUX_WHITE;
    case TUX_MAT_BEAK: case TUX_MAT_NOSTRIL: return PAL_BEAK;
    case TUX_MAT_IRIS: return PAL_TUX_BLACK;
    default: return PAL_TUX_BLACK;
    }
}

void scene_draw_tux(const mat34 *root, const tux_pose_t *pose)
{
    static mat34 frames[TUX_NUM_FRAMES];
    const tux_mesh_t *mesh;
    int f, i;
    vec3 c;
    /* light direction in world space; per frame we bring it into frame
       space (M^T * L) so normals need no transformation */
    tux_eval_frames(pose, root, frames);
    cam_transform(&c, &root->t);
    if (c.z < -FX_ONE) return;
    mesh = c.z > FX(9.0) ? tux_mesh_lod1 : tux_mesh_lod0;
    for (f = 0; f < TUX_NUM_FRAMES; f++) {
        const mat34 *M = &frames[f];
        const tux_mesh_t *m = &mesh[f];
        mat34 MC;                    /* frame -> camera in one matrix */
        vec3 lf;
        fx inv_scale;
        int n = m->num_verts;
        if (n == 0 || n > TUX_MAX_FRAME_VERTS) continue;
        /* frame-space light: M^T * light (M has uniform scale 0.35) */
        m34_apply_vec_t(&lf, M, &g_light_dir);
        /* |M col| = scale; normalise lf once per frame */
        {
            fx s2 = fxmul(M->m[0][0], M->m[0][0]) + fxmul(M->m[1][0], M->m[1][0]) + fxmul(M->m[2][0], M->m[2][0]);
            inv_scale = fx_rsqrt(s2);
            lf.x = fxmul(lf.x, inv_scale); lf.y = fxmul(lf.y, inv_scale); lf.z = fxmul(lf.z, inv_scale);
        }
        /* MC = view * (translate(-cam) * M): 9 multiplies per vertex
           instead of 18 and no function calls */
        {
            mat34 T = *M;
            T.t.x -= g_cam.pos.x; T.t.y -= g_cam.pos.y; T.t.z -= g_cam.pos.z;
            m34_mul(&MC, &g_cam.view, &T);
        }
        for (i = 0; i < n; i++) {
            vec3 *cv = &tux_cam[i];
            m34_apply_i(cv, &MC, &m->verts[i]);
            if (cv->z >= NEAR_Z) {
                project_fast(cv->x, cv->y, cv->z, &tux_sx[i], &tux_sy[i]);
                tux_ok[i] = 1;
            } else {
                tux_ok[i] = 0;
            }
        }
        for (i = 0; i < m->num_tris; i++) {
            const tux_tri_t *t = &m->tris[i];
            int i0 = t->i0, i1 = t->i1, i2 = t->i2;
            fx d;
            int lvl, area;
            u8 col;
            if (!(tux_ok[i0] & tux_ok[i1] & tux_ok[i2])) {
                /* near-plane straddling: fall back to the clipping path */
                render_tri_cam(&tux_cam[i0], &tux_cam[i1], &tux_cam[i2], (u8)(tux_material_base(t->material) + 8), 1);
                continue;
            }
            area = (tux_sx[i1] - tux_sx[i0]) * (tux_sy[i2] - tux_sy[i0]) - (tux_sx[i2] - tux_sx[i0]) * (tux_sy[i1] - tux_sy[i0]);
            if (area >= 0) continue;          /* back face */
            /* shade: dot(n, light), n is s8*127; a bit of wrap-around light
               so the shadow side keeps some shape (Tux Racer uses ambient
               0.45..0.75 which the palette ramps already include) */
            /* n is s8 * 127: (dot / 127 + 0.3) * 12 / 13, without divides
               (516 / 2^16 = 1/127 to 0.01 %) */
            d = (t->n[0] * lf.x + t->n[1] * lf.y + t->n[2] * lf.z) >> 7;
            d = fxmul(d + fxmul(d, FX(0.00787)) + FX(0.3), FX(12.0 / 13.0));
            if (d < 0) d = 0;
            lvl = (d * 15 + FX_HALF) >> FX_SHIFT;
            if (lvl > 15) lvl = 15;
            col = (u8)(tux_material_base(t->material) + lvl);
            /* depth bias so the white belly / beak / eyes win against the
               body sphere they sit on (they are separate ellipsoids) */
            d = (tux_cam[i0].z + tux_cam[i1].z + tux_cam[i2].z) / 3;
            if (t->material == TUX_MAT_WHITE) d -= (f == TUX_FRAME_BODY) ? FX(0.05) : FX(0.02);
            else if (t->material == TUX_MAT_BEAK || t->material == TUX_MAT_NOSTRIL) d -= FX(0.04);
            else if (t->material == TUX_MAT_IRIS) d -= FX(0.10);
            /* the head sits in front of the neck ellipsoid */
            if (f == TUX_FRAME_HEAD) d -= FX(0.03);
            render_push_tri(tux_sx[i0], tux_sy[i0], tux_sx[i1], tux_sy[i1], tux_sx[i2], tux_sy[i2], col, d);
        }
        if (g_rs->num_tris >= MAX_TRIS - 40) break;
    }
}
