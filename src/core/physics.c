/*
 * Tux Racer 32X - player physics.  Faithful port of phys_sim.c (Jasmin
 * Patry, GPLv2) to 16.16 fixed point.  Integration uses the Bogacki-
 * Shampine ode23 scheme (the original's default solver) with the same
 * adaptive step-size control.
 */
#include "physics.h"
#include "tuxmodel.h"

player_t g_player;
game_state_t g_game;

/* --- constants from phys_sim.c ------------------------------------------ */
#define TUX_WIDTH FX(0.45)
#define MIN_FRICTION_SPEED FX(2.8)
#define MAX_FRICTIONAL_FORCE FX(800.0)
#define MAX_TURN_ANGLE FX(45.0)
#define MAX_TURN_PERPENDICULAR_FORCE FX(400.0)
#define MAX_TURN_PENALTY FX(0.15)
#define BRAKE_FORCE FX(200.0)
#define MAX_ROLL_ANGLE FX(30.0)
#define BRAKING_ROLL_ANGLE FX(55.0)
#define IDEAL_ROLL_SPEED FX(6.0)
#define IDEAL_ROLL_FRIC_COEFF FX(0.35)
#define TUX_ORIENTATION_TIME_CONSTANT FX(0.14)
#define TUX_ORIENTATION_AIRBORNE_TIME_CONSTANT FX(0.5)
#define TUX_GLUTE_STAGE_1_COMPRESSIBILITY FX(0.05)
#define TUX_GLUTE_STAGE_1_SPRING_COEFF FX(1500.0)
#define TUX_GLUTE_STAGE_1_DAMPING_COEFF FX(100.0)
#define TUX_GLUTE_STAGE_2_COMPRESSIBILITY FX(0.12)
#define TUX_GLUTE_STAGE_2_SPRING_COEFF FX(3000.0)
#define TUX_GLUTE_STAGE_2_DAMPING_COEFF FX(500.0)
#define TUX_GLUTE_STAGE_3_SPRING_COEFF FX(10000.0)
#define TUX_GLUTE_STAGE_3_DAMPING_COEFF FX(1000.0)
#define TUX_GLUTE_MAX_SPRING_FORCE FX(3000.0)
#define MAX_SURFACE_PENETRATION FX(0.2)
#define MIN_TIME_STEP FX(0.01)
#define MAX_TIME_STEP FX(0.10)
#define MAX_STEP_DISTANCE FX(0.20)
#define MAX_POSITION_ERROR FX(0.005)
#define MAX_VELOCITY_ERROR FX(0.05)
#define TUX_Y_CORRECTION_ON_STOMACH FX(0.33)
#define COLLISION_TOLERANCE FX(0.04)
#define PADDLING_DURATION FX(0.40)
#define MAX_PADDLING_SPEED FX(16.6666667)   /* 60 km/h */
#define IDEAL_PADDLING_FRIC_COEFF FX(0.35)
#define MAX_PADDLING_FORCE FX(122.5)
#define BASE_JUMP_G_FORCE FX(1.5)
#define MAX_JUMP_G_FORCE FX(3.0)
#define JUMP_FORCE_DURATION FX(0.20)
#define MAX_JUMP_AMT FX(1.0)
#define TURN_DECAY_TIME_CONSTANT FX(0.5)
#define ROLL_DECAY_TIME_CONSTANT FX(0.2)

static const fx fric_coeff[NUM_TERRAINS] = { FX(0.22), FX(0.9), FX(0.35) };   /* ice rock snow */
static const fx comp_depth_tab[NUM_TERRAINS] = { FX(0.03), FX(0.01), FX(0.11) };

/* --------------------------------------------------------------- helpers */

typedef struct { vec3 nml; fx d; } plane_t;

static void get_local_course_plane(const vec3 *pt, plane_t *pl)
{
    fx y = course_find_y(pt->x, pt->z);
    course_find_normal(pt->x, pt->z, &pl->nml);
    pl->d = -(fxmul(pl->nml.x, pt->x) + fxmul(pl->nml.y, y) + fxmul(pl->nml.z, pt->z));
}

static fx distance_to_plane(const plane_t *pl, const vec3 *p)
{
    return fxmul(pl->nml.x, p->x) + fxmul(pl->nml.y, p->y) + fxmul(pl->nml.z, p->z) + pl->d;
}

/* rotate v about unit axis by deg -> o */
static void rotate_about(vec3 *o, const vec3 *axis, fx deg, const vec3 *v)
{
    mat34 m;
    if (deg == 0) { *o = *v; return; }
    m34_rot_axis(&m, axis, deg);
    m34_apply_vec(o, &m, v);
}

fx player_speed(void)
{
    return v3_length(&g_player.vel);
}

/* ----------------------------------------------------------- set_tux_pos */

void player_set_pos(const vec3 *p)
{
    vec3 np = *p;
    fx boundary = (g_course.width - g_course.play_width) >> 1;
    if (np.x < boundary) np.x = boundary;
    else if (np.x > g_course.width - boundary) np.x = g_course.width - boundary;
    if (np.z > 0) np.z = 0;
    else if (-np.z >= g_course.play_length) {
        np.z = -g_course.play_length;
        g_game.race_over = 1;
    }
    g_player.pos = np;
}

/* ------------------------------------------------------ tree collisions */

/* Tux Racer intersects the tree polyhedron (a 6-vertex, 8-face diamond
   scaled to diam x height x diam) against every sphere in the Tux model.
   We approximate Tux by a sphere of radius TUX_WIDTH*0.75 (~0.34 m) at
   his position and test that against the same polyhedron faces.  The
   original's own broad-phase uses diam/2+0.6. */
static const s8 tree_poly_verts[6][3] = {   /* *2 : {0,0,0},{0,.15,.5},{.5,.15,0},{0,.15,-.5},{-.5,.15,0},{0,1,0} */
    {0, 0, 0}, {0, 15, 50}, {50, 15, 0}, {0, 15, -50}, {-50, 15, 0}, {0, 100, 0}
};
static const u8 tree_poly_tris[8][3] = {
    {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1},
    {5, 2, 1}, {5, 3, 2}, {5, 4, 3}, {5, 1, 4}
};

static int sphere_hits_tri(const vec3 *c, fx r, const vec3 *a, const vec3 *b, const vec3 *d)
{
    /* distance from c to triangle (a,b,d) <= r ?  Uses plane distance + edge
       tests, mirroring intersect_polygon on a unit sphere. */
    vec3 e1, e2, n, ac, pt, en, tmp;
    fx dist, r2 = fxmul(r, r);
    int i;
    const vec3 *v[3];
    v[0] = a; v[1] = b; v[2] = d;
    v3_sub(&e1, b, a);
    v3_sub(&e2, d, a);
    v3_cross(&n, &e1, &e2);
    if (v3_normalize(&n) == 0) return 0;
    v3_sub(&ac, c, a);
    dist = v3_dot(&ac, &n);
    if (fabsx(dist) > r) return 0;
    /* edges */
    for (i = 0; i < 3; i++) {
        const vec3 *v0 = v[i], *v1 = v[(i + 1) % 3];
        vec3 ev, w;
        fx len, t, d2;
        v3_sub(&ev, v1, v0);
        len = v3_normalize(&ev);
        v3_sub(&w, c, v0);
        t = v3_dot(&w, &ev);
        if (t < 0) { d2 = v3_dot(&w, &w); }
        else if (t > len) { v3_sub(&w, c, v1); d2 = v3_dot(&w, &w); }
        else { v3_madd(&tmp, v0, &ev, t); v3_sub(&w, c, &tmp); d2 = v3_dot(&w, &w); }
        if (d2 <= r2) return 1;
    }
    /* projected point inside triangle? */
    v3_madd(&pt, c, &n, -dist);
    for (i = 0; i < 3; i++) {
        vec3 edge, pv;
        v3_sub(&edge, v[(i + 1) % 3], v[i]);
        v3_cross(&en, &n, &edge);
        v3_sub(&pv, &pt, v[i]);
        if (v3_dot(&pv, &en) < 0) return 0;
    }
    return 1;
}

static int check_tree_collisions(const vec3 *pos, vec3 *tree_loc, fx *tree_diam)
{
    const course_def_t *d = g_course.def;
    int i;
    fx tux_r = FX(0.34);
    for (i = 0; i < d->num_trees; i++) {
        const tree_def_t *t = &d->trees[i];
        fx tx = POS11_5_TO_FX(t->x), tz = POS11_5_TO_FX(t->z);
        fx diam = (fx)t->diam << 8, height = (fx)t->height << 8;
        fx dx = pos->x - tx, dz = pos->z - tz;
        fx rr = (diam >> 1) + FX(0.6);
        vec3 verts[6], loc;
        int k;
        /* trees sorted by z descending: skip the ones far downhill early */
        if (tz < pos->z - FX(6.0)) break;
        if (fabsx(dx) > rr || fabsx(dz) > rr) continue;
        if (fxmul(dx, dx) + fxmul(dz, dz) > fxmul(rr, rr)) continue;
        loc.x = tx; loc.z = tz; loc.y = course_find_y(tx, tz);
        for (k = 0; k < 6; k++) {
            verts[k].x = loc.x + fxmul(diam, tree_poly_verts[k][0] * 655);   /* /100 */
            verts[k].y = loc.y + fxmul(height, tree_poly_verts[k][1] * 655);
            verts[k].z = loc.z + fxmul(diam, tree_poly_verts[k][2] * 655);
        }
        for (k = 0; k < 8; k++) {
            if (sphere_hits_tri(pos, tux_r, &verts[tree_poly_tris[k][0]], &verts[tree_poly_tris[k][1]], &verts[tree_poly_tris[k][2]])) {
                *tree_loc = loc;
                *tree_diam = diam;
                g_player.collision = 1;
                return 1;
            }
        }
    }
    return 0;
}

static void adjust_for_tree_collision(const vec3 *pos, vec3 *vel)
{
    vec3 tree_nml, loc;
    fx diam, speed, costheta;
    if (!check_tree_collisions(pos, &loc, &diam)) return;
    tree_nml.x = pos->x - loc.x;
    tree_nml.y = 0;
    tree_nml.z = pos->z - loc.z;
    v3_normalize(&tree_nml);
    speed = v3_normalize(vel);
    speed = fxmul(speed, FX(0.7));
    costheta = v3_dot(vel, &tree_nml);
    if (costheta < 0) {
        speed = fxmul(speed, FX_ONE + costheta);
        speed = fxmul(speed, FX_ONE + costheta);
        v3_madd(vel, vel, &tree_nml, -2 * costheta);
        v3_normalize(vel);
    }
    if (speed < MIN_TUX_SPEED) speed = MIN_TUX_SPEED;
    v3_scale(vel, vel, speed);
}

/* -------------------------------------------------------- item pickups */
int g_herring_pickup_event;

static void check_item_collection(const vec3 *pos)
{
    const course_def_t *d = g_course.def;
    int i;
    for (i = 0; i < d->num_items && i < MAX_ITEMS; i++) {
        const item_def_t *it = &d->items[i];
        fx ix, iz, iy, diam, height, dx, dz, rr;
        if (!it->collectable || g_course.item_collected[i]) continue;
        iz = POS11_5_TO_FX(it->z);
        if (iz < pos->z - FX(4.0)) break;         /* sorted by z descending */
        ix = POS11_5_TO_FX(it->x);
        diam = (fx)it->diam << 8;
        height = (fx)it->height << 8;
        dx = pos->x - ix; dz = pos->z - iz;
        rr = (diam >> 1) + FX(0.6);
        if (fabsx(dx) > rr || fabsx(dz) > rr) continue;
        if (fxmul(dx, dx) + fxmul(dz, dz) > fxmul(rr, rr)) continue;
        iy = course_find_y(ix, iz) + ((fx)it->above << 8);
        if ((pos->y - FX(0.6) >= iy && pos->y - FX(0.6) <= iy + height) ||
            (pos->y + FX(0.6) >= iy && pos->y + FX(0.6) <= iy + height) ||
            (pos->y - FX(0.6) <= iy && pos->y + FX(0.6) >= iy + height)) {
            g_course.item_collected[i] = 1;
            g_player.herring++;
            g_herring_pickup_event = 1;
        }
    }
}

/* ------------------------------------------------------------- forces */

static void calc_spring_force(vec3 *out, fx compression, const vec3 *vel, const vec3 *surf_nml, vec3 *unclamped)
{
    fx spring_vel = v3_dot(vel, surf_nml);
    fx f, damp;
    f = fxmul(fminx(compression, TUX_GLUTE_STAGE_1_COMPRESSIBILITY), TUX_GLUTE_STAGE_1_SPRING_COEFF);
    f += fxmul(fmaxx(0, fminx(compression - TUX_GLUTE_STAGE_1_COMPRESSIBILITY, TUX_GLUTE_STAGE_2_COMPRESSIBILITY)),
               TUX_GLUTE_STAGE_2_SPRING_COEFF);
    f += fxmul(fmaxx(0, compression - TUX_GLUTE_STAGE_2_COMPRESSIBILITY - TUX_GLUTE_STAGE_1_COMPRESSIBILITY),
               TUX_GLUTE_STAGE_3_SPRING_COEFF);
    damp = compression <= TUX_GLUTE_STAGE_1_COMPRESSIBILITY ? TUX_GLUTE_STAGE_1_SPRING_COEFF
         : (compression <= TUX_GLUTE_STAGE_2_COMPRESSIBILITY ? TUX_GLUTE_STAGE_2_DAMPING_COEFF : TUX_GLUTE_STAGE_3_DAMPING_COEFF);
    f -= fxmul(spring_vel, damp);
    if (f < 0) f = 0;
    v3_scale(unclamped, surf_nml, f);
    if (f > TUX_GLUTE_MAX_SPRING_FORCE) f = TUX_GLUTE_MAX_SPRING_FORCE;
    v3_scale(out, surf_nml, f);
}

static void adjust_surf_nml_for_roll(vec3 *out, const vec3 *vel_in, fx fric, const vec3 *nml)
{
    vec3 vel = *vel_in, proj;
    fx speed = v3_normalize(&vel);
    fx roll_angle = g_player.control.is_braking ? BRAKING_ROLL_ANGLE : MAX_ROLL_ANGLE;
    fx angle, f1, f2;
    v3_project_plane(&proj, nml, &vel);
    v3_normalize(&proj);
    f1 = fminx(FX_ONE, fxdiv(fmaxx(0, fric), IDEAL_ROLL_FRIC_COEFF));
    f2 = fminx(FX_ONE, fxdiv(fmaxx(0, speed - MIN_TUX_SPEED), IDEAL_ROLL_SPEED - MIN_TUX_SPEED));
    angle = fxmul(fxmul(fxmul(g_player.control.turn_fact, roll_angle), f1), f2);
    rotate_about(out, &proj, angle, nml);
}

static void adjust_tux_zvec_for_roll(vec3 *out, const vec3 *vel_in, const vec3 *zvec)
{
    vec3 vel;
    fx ang;
    v3_project_plane(&vel, zvec, vel_in);
    v3_normalize(&vel);
    ang = fxmul(g_player.control.turn_fact, g_player.control.is_braking ? BRAKING_ROLL_ANGLE : MAX_ROLL_ANGLE);
    rotate_about(out, &vel, ang, zvec);
}

static void calc_wind_force(vec3 *out, const vec3 *vel)
{
    vec3 total;
    fx speed, f;
    int idx;
    total.x = -vel->x; total.y = -vel->y; total.z = -vel->z;
    speed = v3_normalize(&total);
    /* table: force at speed idx*0.5 m/s, linear interpolate */
    idx = speed >> 15;                            /* speed / 0.5 */
    if (idx >= 256) { f = drag_force_tab[256]; }
    else {
        fx frac = (speed - (idx << 15)) << 1;    /* 0..1 in 16.16 */
        f = drag_force_tab[idx] + fxmul(drag_force_tab[idx + 1] - drag_force_tab[idx], frac);
    }
    v3_scale(out, &total, f);
}

static void update_paddling(void)
{
    if (g_player.control.is_paddling) {
        if (g_game.time - g_player.control.paddle_time >= PADDLING_DURATION)
            g_player.control.is_paddling = 0;
    }
}

static void calc_net_force(vec3 *net, const vec3 *pos, const vec3 *vel)
{
    vec3 nml_f = {0, 0, 0}, unclamped = {0, 0, 0}, fric_f = {0, 0, 0}, grav_f, air_f, brake_f = {0, 0, 0};
    vec3 paddling_f = {0, 0, 0}, jump_f = {0, 0, 0};
    vec3 fric_dir, surf_nml, orig_nml;
    fx comp_depth, speed, fric_coef, dist;
    fx weights[NUM_TERRAINS];
    plane_t pl;
    int i;
    player_t *p = &g_player;

    course_surface_weights(pos->x, pos->z, weights);
    get_local_course_plane(pos, &pl);
    orig_nml = pl.nml;
    fric_coef = 0;
    comp_depth = 0;
    for (i = 0; i < NUM_TERRAINS; i++) {
        fric_coef += fxmul(weights[i], fric_coeff[i]);
        comp_depth += fxmul(weights[i], comp_depth_tab[i]);
    }
    adjust_surf_nml_for_roll(&surf_nml, vel, fric_coef, &orig_nml);

    v3_set(&grav_f, 0, -fxmul(EARTH_GRAV, TUX_MASS), 0);
    dist = distance_to_plane(&pl, pos);
    p->airborne = dist > 0;

    if (dist <= -comp_depth) {
        fx compression = -dist - comp_depth;
        calc_spring_force(&nml_f, compression, vel, &surf_nml, &unclamped);
    }

    if (p->control.begin_jump) {
        p->control.begin_jump = 0;
        if (dist <= 0) {
            p->control.jumping = 1;
            p->control.jump_start_time = g_game.time;
        } else {
            p->control.jumping = 0;
        }
    }
    if (p->control.jumping && g_game.time - p->control.jump_start_time < JUMP_FORCE_DURATION) {
        fx mg = fxmul(TUX_MASS, EARTH_GRAV);
        jump_f.y = fxmul(BASE_JUMP_G_FORCE, mg) + fxmul(fxmul(p->control.jump_amt, MAX_JUMP_G_FORCE - BASE_JUMP_G_FORCE), mg);
    } else {
        p->control.jumping = 0;
    }
    p->normal_force = unclamped;

    fric_dir = *vel;
    speed = v3_normalize(&fric_dir);
    fric_dir.x = -fric_dir.x; fric_dir.y = -fric_dir.y; fric_dir.z = -fric_dir.z;

    if (dist < 0 && speed > MIN_FRICTION_SPEED) {
        fx fric_mag = fxmul(v3_length(&nml_f), fric_coef);
        fx steer_angle, s;
        if (fric_mag > MAX_FRICTIONAL_FORCE) fric_mag = MAX_FRICTIONAL_FORCE;
        v3_scale(&fric_f, &fric_dir, fric_mag);
        steer_angle = fxmul(p->control.turn_fact, MAX_TURN_ANGLE);
        s = fxmul(fric_mag, fx_sin_deg(steer_angle));
        if (fabsx(s) > MAX_TURN_PERPENDICULAR_FORCE) {
            fx a = fx_asin_deg(fxdiv(MAX_TURN_PERPENDICULAR_FORCE, fric_mag));
            steer_angle = p->control.turn_fact > 0 ? a : -a;
        }
        {
            vec3 tmp;
            rotate_about(&tmp, &orig_nml, steer_angle, &fric_f);
            v3_scale(&fric_f, &tmp, FX_ONE + MAX_TURN_PENALTY);
        }
        if (speed > MIN_TUX_SPEED && p->control.is_braking) {
            v3_scale(&brake_f, &fric_dir, fxmul(fric_coef, BRAKE_FORCE));
        }
    }

    calc_wind_force(&air_f, vel);

    update_paddling();
    if (p->control.is_paddling) {
        if (p->airborne) {
            vec3 lf;
            v3_set(&lf, 0, 0, -(fxmul(TUX_MASS, EARTH_GRAV) >> 2));
            q_rotate(&paddling_f, &p->orientation, &lf);
        } else {
            fx f = fxmul(fxmul(MAX_PADDLING_FORCE, fxdiv(MAX_PADDLING_SPEED - speed, MAX_PADDLING_SPEED)),
                         fminx(FX_ONE, fxdiv(fric_coef, IDEAL_PADDLING_FRIC_COEFF)));
            if (f > MAX_PADDLING_FORCE) f = MAX_PADDLING_FORCE;
            v3_scale(&paddling_f, &fric_dir, -f);
        }
    }

    net->x = jump_f.x + grav_f.x + nml_f.x + fric_f.x + air_f.x + brake_f.x + paddling_f.x;
    net->y = jump_f.y + grav_f.y + nml_f.y + fric_f.y + air_f.y + brake_f.y + paddling_f.y;
    net->z = jump_f.z + grav_f.z + nml_f.z + fric_f.z + air_f.z + brake_f.z + paddling_f.z;
}

static fx adjust_time_step_size(fx h, const vec3 *vel)
{
    fx speed = v3_length(vel);
    if (h < MIN_TIME_STEP) h = MIN_TIME_STEP;
    if (speed > 0) {
        fx hmax = fxdiv(MAX_STEP_DISTANCE, speed);
        if (h > hmax) h = hmax;
    }
    if (h > MAX_TIME_STEP) h = MAX_TIME_STEP;
    return h;
}

/* --------------------------------------------------------- ODE solver */
/* Bogacki-Shampine (ode23) as in nmrcl.c:
   time steps 0, 1/2, 3/4, 1
   k0 = h f(y0)
   y1 = y0 + 1/2 k0
   y2 = y0 + 3/4 k1
   y3 = y0 + 2/9 k0 + 1/3 k1 + 4/9 k2   (final)
   k3 = h f(y3)
   err = -5/72 k0 + 1/12 k1 + 1/9 k2 - 1/8 k3 */

typedef struct { vec3 pos, vel; } state_t;

static void state_axpy(state_t *o, const state_t *y0, const state_t *k0, fx a0, const state_t *k1, fx a1, const state_t *k2, fx a2)
{
    o->pos.x = y0->pos.x + fxmul(k0->pos.x, a0) + fxmul(k1->pos.x, a1) + fxmul(k2->pos.x, a2);
    o->pos.y = y0->pos.y + fxmul(k0->pos.y, a0) + fxmul(k1->pos.y, a1) + fxmul(k2->pos.y, a2);
    o->pos.z = y0->pos.z + fxmul(k0->pos.z, a0) + fxmul(k1->pos.z, a1) + fxmul(k2->pos.z, a2);
    o->vel.x = y0->vel.x + fxmul(k0->vel.x, a0) + fxmul(k1->vel.x, a1) + fxmul(k2->vel.x, a2);
    o->vel.y = y0->vel.y + fxmul(k0->vel.y, a0) + fxmul(k1->vel.y, a1) + fxmul(k2->vel.y, a2);
    o->vel.z = y0->vel.z + fxmul(k0->vel.z, a0) + fxmul(k1->vel.z, a1) + fxmul(k2->vel.z, a2);
}

/* k = h * f(state) where f = (vel, force/m) */
static void deriv(state_t *k, const state_t *s, const vec3 *force, fx h)
{
    fx hm = fxdiv(h, TUX_MASS);
    v3_scale(&k->pos, &s->vel, h);
    v3_scale(&k->vel, force, hm);
}

static const state_t zero_state = { {0, 0, 0}, {0, 0, 0} };

static void solve_ode_system(fx dtime)
{
    player_t *p = &g_player;
    fx h = p->ode_time_step;
    fx t = 0;
    int done = 0, failed;
    state_t y, saved;
    vec3 f, saved_f;
    fx err = 0, tol = FX_ONE;
    int guard = 0;

    if (h < 0) h = adjust_time_step_size(dtime, &p->vel);

    y.pos = p->pos;
    y.vel = p->vel;
    f = p->net_force;

    while (!done && guard++ < 64) {
        state_t k0, k1, k2, k3, y1, y2, y3, e;
        vec3 f1, f2, f3;
        fx pos_err, vel_err;

        if (t >= dtime) break;
        if (fxmul(FX(1.1), h) > dtime - t) {
            h = dtime - t;
            done = 1;
        }
        saved = y;
        saved_f = f;
        failed = 0;
        for (;;) {
            deriv(&k0, &y, &f, h);
            state_axpy(&y1, &y, &k0, FX(0.5), &zero_state, 0, &zero_state, 0);
            calc_net_force(&f1, &y1.pos, &y1.vel);
            deriv(&k1, &y1, &f1, h);
            state_axpy(&y2, &y, &k1, FX(0.75), &zero_state, 0, &zero_state, 0);
            calc_net_force(&f2, &y2.pos, &y2.vel);
            deriv(&k2, &y2, &f2, h);
            state_axpy(&y3, &y, &k0, FX(2.0 / 9.0), &k1, FX(1.0 / 3.0), &k2, FX(4.0 / 9.0));
            calc_net_force(&f3, &y3.pos, &y3.vel);
            deriv(&k3, &y3, &f3, h);
            /* error estimate */
            state_axpy(&e, &zero_state, &k0, FX(-5.0 / 72.0), &k1, FX(1.0 / 12.0), &k2, FX(1.0 / 9.0));
            e.pos.x -= k3.pos.x >> 3; e.pos.y -= k3.pos.y >> 3; e.pos.z -= k3.pos.z >> 3;
            e.vel.x -= k3.vel.x >> 3; e.vel.y -= k3.vel.y >> 3; e.vel.z -= k3.vel.z >> 3;
            pos_err = v3_length(&e.pos);
            vel_err = v3_length(&e.vel);
            /* compare pos_err/MAX_POS vs vel_err/MAX_VEL  <=> pos_err*10 vs vel_err */
            if (pos_err * 10 > vel_err) { err = pos_err; tol = MAX_POSITION_ERROR; }
            else { err = vel_err; tol = MAX_VELOCITY_ERROR; }
            y = y3;
            f = f3;
            if (err > tol && h > MIN_TIME_STEP + 1) {
                done = 0;
                if (!failed) {
                    /* h *= max(0.5, 0.8 * (tol/err)^(1/3)) */
                    fx ratio = fxdiv(tol, err);          /* < 1 */
                    fx cbrt;
                    /* cube root via two Newton steps from sqrt guess */
                    cbrt = fx_sqrt(ratio);
                    if (cbrt > 0) {
                        int it;
                        for (it = 0; it < 3; it++) {
                            fx c2 = fxmul(cbrt, cbrt);
                            if (c2 == 0) break;
                            cbrt = fxmul(FX(2.0 / 3.0), cbrt) + fxmul(FX(1.0 / 3.0), fxdiv(ratio, c2));
                        }
                    }
                    failed = 1;
                    h = fxmul(h, fmaxx(FX(0.5), fxmul(FX(0.8), cbrt)));
                } else {
                    h >>= 1;
                }
                h = adjust_time_step_size(h, &saved.vel);
                y = saved;
                f = saved_f;
                if (guard++ > 64) break;
            } else {
                break;
            }
        }
        t += h;
        /* final force at the new state */
        calc_net_force(&f, &y.pos, &y.vel);
        if (!failed) {
            /* temp = 1.25 * (err/tol)^(1/3) */
            fx ratio = tol > 0 ? fxdiv(err, tol) : 0;
            fx cbrt = fx_sqrt(ratio);
            int it;
            for (it = 0; it < 3 && cbrt > 0; it++) {
                fx c2 = fxmul(cbrt, cbrt);
                if (c2 == 0) break;
                cbrt = fxmul(FX(2.0 / 3.0), cbrt) + fxmul(FX(1.0 / 3.0), fxdiv(ratio, c2));
            }
            {
                fx temp = fxmul(FX(1.25), cbrt);
                if (temp > FX(0.2)) h = fxdiv(h, temp);
                else h = h * 5;
            }
        }
        h = adjust_time_step_size(h, &y.vel);
        adjust_for_tree_collision(&y.pos, &y.vel);
        check_item_collection(&y.pos);
    }
    p->ode_time_step = h;
    p->vel = y.vel;
    p->pos = y.pos;
    p->net_force = f;
}

/* -------------------------------------------------------- orientation */

static void adjust_orientation(fx dtime, const vec3 *vel, fx dist_from_surface, const vec3 *surf_nml)
{
    player_t *p = &g_player;
    vec3 new_x, new_y, new_z, tmp;
    mat34 cob;
    quat new_orient;
    fx tc, alpha;
    static const vec3 minus_z = { 0, 0, -FX_ONE };
    static const vec3 y_vec = { 0, FX_ONE, 0 };

    if (dist_from_surface > 0) {
        vec3 down = { 0, -FX_ONE, 0 };
        new_y = *vel;
        v3_normalize(&new_y);
        v3_project_plane(&tmp, &new_y, &down);
        v3_normalize(&tmp);
        adjust_tux_zvec_for_roll(&new_z, vel, &tmp);
    } else {
        tmp.x = -surf_nml->x; tmp.y = -surf_nml->y; tmp.z = -surf_nml->z;
        adjust_tux_zvec_for_roll(&new_z, vel, &tmp);
        v3_project_plane(&new_y, surf_nml, vel);
        v3_normalize(&new_y);
    }
    v3_cross(&new_x, &new_y, &new_z);
    /* change of basis: columns are new_x, new_y, new_z */
    cob.m[0][0] = new_x.x; cob.m[1][0] = new_x.y; cob.m[2][0] = new_x.z;
    cob.m[0][1] = new_y.x; cob.m[1][1] = new_y.y; cob.m[2][1] = new_y.z;
    cob.m[0][2] = new_z.x; cob.m[1][2] = new_z.y; cob.m[2][2] = new_z.z;
    cob.t.x = cob.t.y = cob.t.z = 0;
    q_from_mat(&new_orient, &cob);
    if (!p->orientation_initialized) {
        p->orientation_initialized = 1;
        p->orientation = new_orient;
    }
    tc = dist_from_surface > 0 ? TUX_ORIENTATION_AIRBORNE_TIME_CONSTANT : TUX_ORIENTATION_TIME_CONSTANT;
    alpha = fxdiv(dtime, tc);
    if (alpha > FX_ONE) alpha = FX_ONE;
    q_nlerp(&p->orientation, &p->orientation, &new_orient, alpha);
    q_rotate(&p->plane_nml, &p->orientation, &minus_z);
    q_rotate(&p->direction, &p->orientation, &y_vec);
}

/* ----------------------------------------------------- update_player_pos */

static void update_player_pos(fx dtime)
{
    player_t *p = &g_player;
    plane_t pl;
    fx dist, speed;
    vec3 tmp;
    quat conj;

    if (dtime > 0) solve_ode_system(dtime);

    get_local_course_plane(&p->pos, &pl);
    dist = distance_to_plane(&pl, &p->pos);

    /* adjust_velocity */
    tmp = p->vel;
    speed = v3_normalize(&tmp);
    if (speed < FX(0.0001)) {
        if (fabsx(pl.nml.x) + fabsx(pl.nml.z) > FX(0.0001)) {
            vec3 down = { 0, -FX_ONE, 0 };
            v3_project_plane(&tmp, &pl.nml, &down);
            v3_normalize(&tmp);
        } else {
            v3_set(&tmp, 0, 0, -FX_ONE);
        }
    }
    if (speed < MIN_TUX_SPEED) speed = MIN_TUX_SPEED;
    v3_scale(&p->vel, &tmp, speed);

    /* adjust_position */
    if (dist < -MAX_SURFACE_PENETRATION) {
        v3_madd(&p->pos, &p->pos, &pl.nml, -MAX_SURFACE_PENETRATION - dist);
    }
    player_set_pos(&p->pos);
    adjust_orientation(dtime, &p->vel, dist, &pl.nml);

    p->anim_speed = speed;
    p->anim_flap_factor = 0;
    p->anim_paddling_factor = 0;
    if (p->control.is_paddling) {
        fx factor = fxdiv(g_game.time - p->control.paddle_time, PADDLING_DURATION);
        if (p->airborne) p->anim_flap_factor = factor;
        else p->anim_paddling_factor = factor;
    }
    q_conj(&conj, &p->orientation);
    q_rotate(&p->anim_local_force, &conj, &p->net_force);
    if (p->control.jumping) {
        p->anim_flap_factor = fxdiv(g_game.time - p->control.jump_start_time, JUMP_FORCE_DURATION);
    }
}

/* ------------------------------------------------------------- init */

void physics_init(void)
{
    player_t *p = &g_player;
    vec3 nml;
    mat34 rot;
    fx y;
    p->pos.x = g_course.def->start_x;
    p->pos.z = g_course.def->start_z;
    y = course_find_y(p->pos.x, p->pos.z);
    course_find_normal(p->pos.x, p->pos.z, &nml);
    m34_rot_x(&rot, FX(-90.0));
    m34_apply_vec(&p->vel, &rot, &nml);
    v3_scale(&p->vel, &p->vel, INIT_TUX_SPEED);
    p->pos.y = y;
    v3_set(&p->net_force, 0, 0, 0);
    v3_set(&p->normal_force, 0, 0, 0);
    p->control.turn_fact = 0;
    p->control.turn_animation = 0;
    p->control.is_braking = 0;
    p->control.is_paddling = 0;
    p->control.jumping = 0;
    p->control.jump_charging = 0;
    p->control.begin_jump = 0;
    p->control.jump_amt = 0;
    p->orientation_initialized = 0;
    p->orientation.x = p->orientation.y = p->orientation.z = 0; p->orientation.w = FX_ONE;
    p->plane_nml = nml;
    p->direction = p->vel;
    p->airborne = 0;
    p->collision = 0;
    p->ode_time_step = -1;
    p->last_terrain = 0;
    p->anim_speed = INIT_TUX_SPEED;
    p->anim_paddling_factor = p->anim_flap_factor = 0;
    v3_set(&p->anim_local_force, 0, 0, 0);
}

/* -------------------------------------------------------- racing_loop */

static void calc_jump_amt(void)
{
    player_t *p = &g_player;
    if (p->control.jump_charging) {
        p->control.jump_amt = fminx(MAX_JUMP_AMT, g_game.time - p->control.charge_start_time);
    } else if (p->control.jumping) {
        p->control.jump_amt = fxmul(p->control.jump_amt,
                                    FX_ONE - fxdiv(g_game.time - p->control.jump_start_time, JUMP_FORCE_DURATION));
        if (p->control.jump_amt < 0) p->control.jump_amt = 0;
    } else {
        p->control.jump_amt = 0;
    }
}

void racing_update(const input_t *in, fx dt)
{
    player_t *p = &g_player;
    int airborne;
    fx weights[NUM_TERRAINS];
    int new_terrain = 0;

    airborne = p->pos.y > course_find_y(p->pos.x, p->pos.z) + JUMP_MAX_START_HEIGHT;
    if (airborne) {
        new_terrain = 1 << NUM_TERRAINS;
    } else {
        course_surface_weights(p->pos.x, p->pos.z, weights);
        if (weights[TERRAIN_SNOW] > 0) new_terrain |= 1 << TERRAIN_SNOW;
        if (weights[TERRAIN_ROCK] > 0) new_terrain |= 1 << TERRAIN_ROCK;
        if (weights[TERRAIN_ICE] > 0) new_terrain |= 1 << TERRAIN_ICE;
    }
    p->last_terrain = new_terrain;

    p->control.is_braking = in->brake;

    calc_jump_amt();
    if (in->charge && !p->control.jump_charging && !p->control.jumping) {
        p->control.jump_charging = 1;
        p->control.charge_start_time = g_game.time;
    }
    if (!in->charge && p->control.jump_charging) {
        p->control.jump_charging = 0;
        p->control.begin_jump = 1;
    }

    if (in->left ^ in->right) {
        int turning_left = in->left;
        fx step = fxmul(FX(0.15), fxdiv(dt, FX(0.05)));
        p->control.turn_fact = turning_left ? -FX_ONE : FX_ONE;
        p->control.turn_animation += turning_left ? -step : step;
        p->control.turn_animation = fclamp(p->control.turn_animation, -FX_ONE, FX_ONE);
    } else {
        p->control.turn_fact = 0;
        if (dt < ROLL_DECAY_TIME_CONSTANT) {
            p->control.turn_animation = fxmul(p->control.turn_animation, FX_ONE - fxdiv(dt, ROLL_DECAY_TIME_CONSTANT));
        } else {
            p->control.turn_animation = 0;
        }
    }

    if (in->paddle && !p->control.is_paddling) {
        p->control.is_paddling = 1;
        p->control.paddle_time = g_game.time;
    }

    update_player_pos(dt);
    g_game.time += dt;
}
