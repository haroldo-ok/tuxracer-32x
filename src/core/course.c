/*
 * Tux Racer 32X - course runtime: elevation, normals, barycentric lookups.
 * Port of the terrain parts of course_load.c and phys_sim.c.
 */
#include "course.h"

course_t g_course;

void course_load(int index)
{
    const course_def_t *d = &course_defs[index];
    int i;
    g_course.def = d;
    g_course.nx = d->nx;
    g_course.ny = d->ny;
    g_course.width = d->width;
    g_course.length = d->length;
    g_course.play_width = d->play_width;
    g_course.play_length = d->play_length;
    g_course.elev_scale_255 = fxdiv(d->elev_scale, FX(255.0));
    /* (ny-1-y)/ny * length * slope  ->  per-row drop = length*slope/ny */
    g_course.dz_per_row = fxdiv(fxmul(d->length, d->slope), FX_FROM_INT(d->ny));
    g_course.xcd_step = fxdiv(d->width, FX_FROM_INT(d->nx - 1));
    g_course.zcd_step = fxdiv(d->length, FX_FROM_INT(d->ny - 1));
    g_course.inv_xcd_step = fxdiv(FX_FROM_INT(d->nx - 1), d->width);
    g_course.inv_zcd_step = fxdiv(FX_FROM_INT(d->ny - 1), d->length);
    g_course.herring_total = 0;
    for (i = 0; i < MAX_ITEMS; i++) g_course.item_collected[i] = 0;
    for (i = 0; i < d->num_items && i < MAX_ITEMS; i++) {
        if (d->items[i].collectable) g_course.herring_total++;
    }
}

fx course_elev(int x, int y)
{
    const course_def_t *d = g_course.def;
    int pix = d->elev[x + d->nx * y] - d->base_height;
    /* ELEV = pix/255*scale - y/ny*length*slope  (with y already flipped) */
    return fxmul(FX_FROM_INT(pix), g_course.elev_scale_255) - fxmul(FX_FROM_INT(y), g_course.dz_per_row);
}

int course_terrain_at(int x, int y)
{
    int idx = x + g_course.nx * y;
    return (g_course.def->terrain[idx >> 2] >> ((idx & 3) * 2)) & 3;
}

extern const fx nml_y_tab[127];

void course_normal(int x, int y, vec3 *n)
{
    const s8 *p = g_course.def->normals + 2 * (x + g_course.nx * y);
    int q = p[0] * p[0] + p[1] * p[1];
    if (q > 16128) q = 16128;
    /* value*127 -> 16.16: v * 65536/127 = v * 516.03 */
    n->x = p[0] * 516;
    n->z = p[1] * 516;
    n->y = nml_y_tab[q >> 7];
}

/* Port of get_indices_for_point + find_barycentric_coords */
void course_barycentric(fx x, fx z, int idx[3][2], fx *u, fx *v)
{
    int nx = g_course.nx, ny = g_course.ny;
    fx xidx = fxmul(x, g_course.inv_xcd_step);
    fx yidx = fxmul(-z, g_course.inv_zcd_step);
    int x0, x1, y0, y1;
    fx dx, dz, ex, ez, qx, qz, det;

    if (xidx < 0) xidx = 0; else if (xidx > FX_FROM_INT(nx - 1)) xidx = FX_FROM_INT(nx - 1);
    if (yidx < 0) yidx = 0; else if (yidx > FX_FROM_INT(ny - 1)) yidx = FX_FROM_INT(ny - 1);

    x0 = FX_INT(xidx);
    x1 = FX_INT(xidx + FX(0.9999));
    y0 = FX_INT(yidx);
    y1 = FX_INT(yidx + FX(0.9999));
    if (x0 == x1) { if (x1 < nx - 1) x1++; else x0--; }
    if (y0 == y1) { if (y1 < ny - 1) y1++; else y0--; }

    if (((x0 + y0) & 1) == 0) {
        if (yidx - FX_FROM_INT(y0) < xidx - FX_FROM_INT(x0)) {
            idx[0][0] = x0; idx[0][1] = y0;
            idx[1][0] = x1; idx[1][1] = y0;
            idx[2][0] = x1; idx[2][1] = y1;
        } else {
            idx[0][0] = x1; idx[0][1] = y1;
            idx[1][0] = x0; idx[1][1] = y1;
            idx[2][0] = x0; idx[2][1] = y0;
        }
    } else {
        if (yidx - FX_FROM_INT(y0) + xidx - FX_FROM_INT(x0) < FX_ONE) {
            idx[0][0] = x0; idx[0][1] = y0;
            idx[1][0] = x1; idx[1][1] = y0;
            idx[2][0] = x0; idx[2][1] = y1;
        } else {
            idx[0][0] = x1; idx[0][1] = y1;
            idx[1][0] = x0; idx[1][1] = y1;
            idx[2][0] = x1; idx[2][1] = y0;
        }
    }
    /* integer differences of grid indices: -1, 0 or 1 */
    dx = FX_FROM_INT(idx[0][0] - idx[2][0]);
    dz = FX_FROM_INT(idx[0][1] - idx[2][1]);
    ex = FX_FROM_INT(idx[1][0] - idx[2][0]);
    ez = FX_FROM_INT(idx[1][1] - idx[2][1]);
    qx = xidx - FX_FROM_INT(idx[2][0]);
    qz = yidx - FX_FROM_INT(idx[2][1]);
    det = fxmul(dx, ez) - fxmul(dz, ex);     /* +-1 */
    if (det > 0) {
        *u = fxmul(qx, ez) - fxmul(qz, ex);
        *v = fxmul(qz, dx) - fxmul(qx, dz);
    } else {
        *u = -(fxmul(qx, ez) - fxmul(qz, ex));
        *v = -(fxmul(qz, dx) - fxmul(qx, dz));
    }
}

fx course_find_y(fx x, fx z)
{
    int idx[3][2];
    fx u, v;
    fx y0, y1, y2;
    course_barycentric(x, z, idx, &u, &v);
    y0 = course_elev(idx[0][0], idx[0][1]);
    y1 = course_elev(idx[1][0], idx[1][1]);
    y2 = course_elev(idx[2][0], idx[2][1]);
    return fxmul(u, y0) + fxmul(v, y1) + fxmul(FX_ONE - u - v, y2);
}

#define NORMAL_INTERPOLATION_LIMIT FX(0.05)

void course_find_normal(fx x, fx z, vec3 *n)
{
    int idx[3][2];
    fx u, v, w, min_bary, interp;
    vec3 n0, n1, n2, smooth, tri, p0, p1, p2, e1, e2;

    course_barycentric(x, z, idx, &u, &v);
    w = FX_ONE - u - v;
    course_normal(idx[0][0], idx[0][1], &n0);
    course_normal(idx[1][0], idx[1][1], &n1);
    course_normal(idx[2][0], idx[2][1], &n2);
    smooth.x = fxmul(u, n0.x) + fxmul(v, n1.x) + fxmul(w, n2.x);
    smooth.y = fxmul(u, n0.y) + fxmul(v, n1.y) + fxmul(w, n2.y);
    smooth.z = fxmul(u, n0.z) + fxmul(v, n1.z) + fxmul(w, n2.z);

    v3_set(&p0, course_xcd(idx[0][0]), course_elev(idx[0][0], idx[0][1]), course_zcd(idx[0][1]));
    v3_set(&p1, course_xcd(idx[1][0]), course_elev(idx[1][0], idx[1][1]), course_zcd(idx[1][1]));
    v3_set(&p2, course_xcd(idx[2][0]), course_elev(idx[2][0], idx[2][1]), course_zcd(idx[2][1]));
    v3_sub(&e1, &p1, &p0);
    v3_sub(&e2, &p2, &p0);
    v3_cross(&tri, &e1, &e2);
    v3_normalize(&tri);
    if (tri.y < 0) { tri.x = -tri.x; tri.y = -tri.y; tri.z = -tri.z; }

    min_bary = fminx(u, fminx(v, w));
    interp = fxdiv(min_bary, NORMAL_INTERPOLATION_LIMIT);
    if (interp > FX_ONE) interp = FX_ONE;
    if (interp < 0) interp = 0;
    /* phys_sim.c: interp_nml = interp*tri_nml + (1-interp)*smooth_nml */
    n->x = fxmul(interp, tri.x) + fxmul(FX_ONE - interp, smooth.x);
    n->y = fxmul(interp, tri.y) + fxmul(FX_ONE - interp, smooth.y);
    n->z = fxmul(interp, tri.z) + fxmul(FX_ONE - interp, smooth.z);
    v3_normalize(n);
}

void course_surface_weights(fx x, fx z, fx w[NUM_TERRAINS])
{
    int idx[3][2];
    fx u, v;
    int t;
    course_barycentric(x, z, idx, &u, &v);
    w[0] = w[1] = w[2] = 0;
    t = course_terrain_at(idx[0][0], idx[0][1]); w[t] += u;
    t = course_terrain_at(idx[1][0], idx[1][1]); w[t] += v;
    t = course_terrain_at(idx[2][0], idx[2][1]); w[t] += FX_ONE - u - v;
}
