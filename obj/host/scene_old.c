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
    int y;
    u8 *fb = g_rs->fb;
    for (y = 0; y < SCREEN_H; y++) {
        int band = y * 8 / SCREEN_H;
        u8 c = (u8)(PAL_SKY + band);
        u16 cc = (u16)((c << 8) | c);
        u16 *p = (u16 *)(fb + y * SCREEN_W);
        int n = SCREEN_W / 2;
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

/* row cache: camera-space positions for two consecutive terrain rows */
#define ROWBUF 128
typedef struct {
    vec3 c[ROWBUF];        /* camera space */
    s8 lvl[ROWBUF];        /* shade level per vertex */
    u8 terr[ROWBUF];
} rowcache_t;

static rowcache_t rows[2];

static void fill_row(rowcache_t *r, int y, int x0, int x1, int step)
{
    int x, i = 0;
    vec3 w;
    w.z = course_zcd(y);
    for (x = x0; x <= x1 && i < ROWBUF; x += step, i++) {
        vec3 n;
        w.x = course_xcd(x);
        w.y = course_elev(x, y);
        cam_transform(&r->c[i], &w);
        course_normal(x, y, &n);
        r->lvl[i] = (s8)shade_level(&n);
        r->terr[i] = (u8)course_terrain_at(x, y);
    }
}

/* distance zones: (max distance m, x step, y step in metres of grid) */
typedef struct { fx max_dist; int xstep_m, ystep_m; } terrain_zone_t;
static const terrain_zone_t zones[4] = {
    { FX(12.0), 1, 1 },
    { FX(28.0), 2, 2 },
    { FX(50.0), 4, 4 },
    { FX(80.0), 8, 8 },
};

static void draw_terrain_zone(int y_from, int y_to, int ystep, int x0, int x1, int xstep, fx far)
{
    int cur = 0, y, count, i;
    if (y_to <= y_from) return;
    x1 = x0 + ((x1 - x0) / xstep) * xstep;
    count = (x1 - x0) / xstep + 1;
    if (count < 2) return;
    fill_row(&rows[cur], y_from, x0, x1, xstep);
    for (y = y_from; y < y_to; y += ystep) {
        int nxt = cur ^ 1;
        int y2 = y + ystep;
        rowcache_t *ra, *rb;
        if (y2 > y_to) y2 = y_to;
        fill_row(&rows[nxt], y2, x0, x1, xstep);
        ra = &rows[cur]; rb = &rows[nxt];
        for (i = 0; i < count - 1; i++) {
            const vec3 *a = &ra->c[i], *b = &ra->c[i + 1], *c = &rb->c[i], *d = &rb->c[i + 1];
            int gx = x0 + i * xstep;
            int lvl1, lvl2, t1, t2;
            fx depth;
            u8 col1, col2;
            if (a->z < NEAR_Z && b->z < NEAR_Z && c->z < NEAR_Z && d->z < NEAR_Z) continue;
            if (a->z > far && b->z > far && c->z > far && d->z > far) continue;
            {
                fx ka = fxmul(a->z, FX(0.83)), kb = fxmul(b->z, FX(0.83));
                fx kc = fxmul(c->z, FX(0.83)), kd = fxmul(d->z, FX(0.83));
                if (a->x > ka && b->x > kb && c->x > kc && d->x > kd) continue;
                if (a->x < -ka && b->x < -kb && c->x < -kc && d->x < -kd) continue;
                ka = fxmul(a->z, FX(0.58)); kb = fxmul(b->z, FX(0.58));
                kc = fxmul(c->z, FX(0.58)); kd = fxmul(d->z, FX(0.58));
                if (a->y < -ka && b->y < -kb && c->y < -kc && d->y < -kd) continue;
                if (a->y > ka && b->y > kb && c->y > kc && d->y > kd) continue;
            }
            depth = (a->z + d->z) >> 1;
            if (((gx + y) & 1) == 0 || xstep > 1) {
                lvl1 = (ra->lvl[i] + rb->lvl[i] + rb->lvl[i + 1]) / 3;
                lvl2 = (ra->lvl[i] + rb->lvl[i + 1] + ra->lvl[i + 1]) / 3;
                t1 = rb->terr[i]; t2 = ra->terr[i + 1];
                if (ra->terr[i] == rb->terr[i]) t1 = ra->terr[i];
                if (ra->terr[i] == rb->terr[i + 1]) t2 = ra->terr[i];
                col1 = terrain_color(t1, lvl1, depth);
                col2 = terrain_color(t2, lvl2, depth);
                render_tri_cam(a, c, d, col1, 0);
                render_tri_cam(a, d, b, col2, 0);
            } else {
                lvl1 = (ra->lvl[i] + rb->lvl[i] + ra->lvl[i + 1]) / 3;
                lvl2 = (ra->lvl[i + 1] + rb->lvl[i] + rb->lvl[i + 1]) / 3;
                t1 = ra->terr[i]; t2 = rb->terr[i + 1];
                if (rb->terr[i] == ra->terr[i + 1]) { t1 = rb->terr[i]; t2 = rb->terr[i]; }
                col1 = terrain_color(t1, lvl1, depth);
                col2 = terrain_color(t2, lvl2, depth);
                render_tri_cam(a, c, b, col1, 0);
                render_tri_cam(b, c, d, col2, 0);
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

static void draw_tree(fx x, fx z, fx height, fx diam, int type, fx dist)
{
    vec3 base, top, ring[8], cbase, ctop, cring[8];
    int i, n;
    fx y = course_find_y(x, z);
    fx r = diam >> 1;
    int shade_base;
    int sides = dist > FX(30.0) ? 4 : 8;
    int stepi = 8 / sides;

    base.x = x; base.y = y; base.z = z;
    cam_transform(&cbase, &base);
    if (cbase.z < NEAR_Z - r && cbase.z + height < NEAR_Z) return;

    if (type == 2) {
        /* barren tree: thin trunk + a few branch fins */
        fx tr = fxmul(r, FX(0.18));
        vec3 t0, t1, t2, t3, ct0, ct1, ct2, ct3;
        top.x = x; top.y = y + height; top.z = z;
        cam_transform(&ctop, &top);
        for (i = 0; i < 4; i++) {
            v3_set(&t0, x + fxmul(tr, circ8[i * 2][0]), y, z + fxmul(tr, circ8[i * 2][1]));
            v3_set(&t1, x + fxmul(tr, circ8[(i * 2 + 2) & 7][0]), y, z + fxmul(tr, circ8[(i * 2 + 2) & 7][1]));
            cam_transform(&ct0, &t0);
            cam_transform(&ct1, &t1);
            render_tri_cam(&ct0, &ct1, &ctop, (u8)(PAL_TRUNK + 6 + i * 2), 0);
        }
        /* two crossed branch fins */
        for (i = 0; i < 2; i++) {
            fx bx = i ? r : 0, bz = i ? 0 : r;
            v3_set(&t0, x - bx, y + fxmul(height, FX(0.45)), z - bz);
            v3_set(&t1, x + bx, y + fxmul(height, FX(0.45)), z + bz);
            v3_set(&t2, x, y + fxmul(height, FX(0.9)), z);
            v3_set(&t3, x, y + fxmul(height, FX(0.3)), z);
            cam_transform(&ct0, &t0); cam_transform(&ct1, &t1); cam_transform(&ct2, &t2); cam_transform(&ct3, &t3);
            render_tri_cam(&ct0, &ct2, &ct3, (u8)(PAL_TREE_BARREN + 8 + i * 3), 0);
            render_tri_cam(&ct1, &ct3, &ct2, (u8)(PAL_TREE_BARREN + 6 + i * 3), 0);
        }
        return;
    }

    /* evergreen (tree1) / shrub (tree3): cone (2 stacked for tall trees) */
    shade_base = type == 0 ? PAL_TREE_GREEN : PAL_TREE_GREEN;
    n = (type == 1 && dist < FX(30.0)) ? 2 : 1;
    for (i = 0; i < 8; i++) {
        v3_set(&ring[i], x + fxmul(r, circ8[i][0]), y, z + fxmul(r, circ8[i][1]));
        cam_transform(&cring[i], &ring[i]);
    }
    {
        int layer;
        for (layer = 0; layer < n; layer++) {
            fx ly = layer == 0 ? y : y + fxmul(height, FX(0.35));
            fx lh = n == 1 ? height : (layer == 0 ? fxmul(height, FX(0.65)) : fxmul(height, FX(0.65)));
            fx lr = layer == 0 ? r : fxmul(r, FX(0.7));
            vec3 tp; vec3 ctp;
            v3_set(&tp, x, ly + lh, z);
            cam_transform(&ctp, &tp);
            for (i = 0; i < 8; i += stepi) {
                int j = (i + stepi) & 7;
                vec3 p0, p1, c0, c1;
                int lvl;
                v3_set(&p0, x + fxmul(lr, circ8[i][0]), ly, z + fxmul(lr, circ8[i][1]));
                v3_set(&p1, x + fxmul(lr, circ8[j][0]), ly, z + fxmul(lr, circ8[j][1]));
                cam_transform(&c0, &p0);
                cam_transform(&c1, &p1);
                /* face normal approx: outward radial direction -> shade by light (1,1,0) */
                {
                    fx nx = (circ8[i][0] + circ8[j][0]) >> 1;
                    fx d = fxmul(nx, FX(0.6)) + FX(0.55);     /* radial x + upward tilt */
                    lvl = (d * 15) >> 16;
                    if (lvl < 2) lvl = 2; if (lvl > 15) lvl = 15;
                }
                render_tri_cam(&c1, &c0, &ctp, (u8)(shade_base + lvl), 1);
            }
        }
    }
    /* trunk for tall trees when close */
    if (type == 1 && dist < FX(20.0)) {
        fx tr = fxmul(r, FX(0.15));
        vec3 t0, t1, t2, t3, ct0, ct1, ct2, ct3;
        v3_set(&t0, x - tr, y - FX(0.3), z); v3_set(&t1, x + tr, y - FX(0.3), z);
        v3_set(&t2, x + tr, y + FX(0.4), z); v3_set(&t3, x - tr, y + FX(0.4), z);
        cam_transform(&ct0, &t0); cam_transform(&ct1, &t1); cam_transform(&ct2, &t2); cam_transform(&ct3, &t3);
        render_tri_cam(&ct0, &ct1, &ct2, PAL_TRUNK + 8, 0);
        render_tri_cam(&ct0, &ct2, &ct3, PAL_TRUNK + 8, 0);
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
        w.x = tx; w.y = course_find_y(tx, tz) + FX_ONE; w.z = tz;
        cam_transform(&c, &w);
        if (c.z < -FX(2.0) || c.z > far) continue;
        if (fabsx(c.x) > c.z + FX(4.0)) continue;   /* outside ~90 deg fov */
        dist = c.z;
        if (dist > FX(55.0) && (i & 1)) continue;   /* thin far trees */
        draw_tree(tx, tz, FX8_8_TO_FX(t->height), FX8_8_TO_FX(t->diam), t->type, dist);
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
        for (i = 0; i < n; i++) {
            vec3 w;
            m34_apply(&w, M, &m->verts[i]);
            cam_transform(&tux_cam[i], &w);
            tux_ok[i] = (u8)render_project(&tux_cam[i], &tux_sx[i], &tux_sy[i]);
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
            d = (t->n[0] * lf.x + t->n[1] * lf.y + t->n[2] * lf.z) / 127;
            d = (d + FX(0.3)) * 12 / 13;
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
