/*
 * Tux Racer 32X - course data (converted from tuxracer-data at build time)
 * and terrain queries (port of course_load.c / phys_sim.c terrain code).
 */
#ifndef COURSE_H
#define COURSE_H

#include "fixed.h"

#define TERRAIN_ICE  0
#define TERRAIN_ROCK 1
#define TERRAIN_SNOW 2
#define NUM_TERRAINS 3

#define ITEM_HERRING 0
#define ITEM_FLAG    1
#define ITEM_FINISH  2
#define ITEM_START   3
#define ITEM_FLOAT   4

typedef struct {
    u8  type;          /* 0 tree3 (shrub), 1 tree1, 2 tree2 (barren) */
    s16 x, z;          /* position, 11.5 fixed (m): world = v << 11 */
    s16 height, diam;  /* 8.8 fixed (m) */
} tree_def_t;

typedef struct {
    u8  kind;          /* ITEM_* */
    s16 x, z;          /* 11.5 */
    s16 diam, height;  /* 8.8 */
    s16 above;         /* 8.8 above-ground offset */
    u8  collectable;   /* 1 = herring */
} item_def_t;

typedef struct {
    const char *name;
    int nx, ny;
    fx width, length;
    fx play_width, play_length;
    fx start_x, start_z;         /* start point (z negative = downhill) */
    fx angle_deg;
    fx elev_scale;
    int base_height;
    fx slope;                    /* tan(angle) */
    const u8 *elev;              /* nx*ny raw pixels, index x + nx*y */
    const u8 *terrain;           /* 2 bits per sample */
    const s8 *normals;           /* nx*ny*2: (nx, nz) * 127; ny = sqrt(1-nx^2-nz^2) */
    const u8 *shade;             /* nx*ny: baked shade level 0..15 (palette.h shade_level) */
    const tree_def_t *trees;
    int num_trees;
    const item_def_t *items;
    int num_items;
    int herring_req[4];
    fx time_req[4];
    fx par_time;
} course_def_t;

#define POS11_5_TO_FX(v) ((fx)(v) << 11)
#define FX8_8_TO_FX(v) ((fx)(v) << 8)

extern const course_def_t course_defs[];
extern const int num_course_defs;

/* --- runtime course state --- */
#define MAX_COURSE_NX 100
#define MAX_COURSE_NY 800
#define MAX_ITEMS 512

typedef struct {
    const course_def_t *def;
    int nx, ny;
    fx width, length;
    fx play_width, play_length;
    fx elev_scale_255;           /* elev_scale / 255 */
    fx dz_per_row;               /* length / ny * slope (drop per row) */
    fx xcd_step;                 /* width / (nx-1) */
    fx zcd_step;                 /* length / (ny-1) */
    fx inv_xcd_step, inv_zcd_step;
    u8 item_collected[MAX_ITEMS];
    int herring_total;
} course_t;

extern course_t g_course;

void course_load(int index);
/* elevation of grid vertex (x,y) in metres (16.16) */
fx course_elev(int x, int y);
static inline fx course_xcd(int x) { return fxmul(FX_FROM_INT(x), g_course.xcd_step); }
static inline fx course_zcd(int y) { return -fxmul(FX_FROM_INT(y), g_course.zcd_step); }
int course_terrain_at(int x, int y);
/* vertex normal (smoothed as in calc_normals) */
void course_normal(int x, int y, vec3 *n);
/* height and interpolated normal at world (x,z); port of find_y_coord/find_course_normal */
fx course_find_y(fx x, fx z);
void course_find_normal(fx x, fx z, vec3 *n);
/* terrain weights (ice, rock, snow) at world position (sum = 1) */
void course_surface_weights(fx x, fx z, fx w[NUM_TERRAINS]);
/* barycentric triangle lookup: writes 3 vertex indices (i,j pairs) and u,v */
void course_barycentric(fx x, fx z, int idx[3][2], fx *u, fx *v);

#endif
