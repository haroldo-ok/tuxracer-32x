/*
 * Tux Racer 32X - camera (port of view.c).
 */
#include "physics.h"

#define CAMERA_DISTANCE FX(4.0)
#define CAMERA_ANGLE_ABOVE_SLOPE FX(10.0)
#define PLAYER_ANGLE_IN_CAMERA FX(20.0)
#define MIN_CAMERA_HEIGHT FX(1.5)
#define ABSOLUTE_MIN_CAMERA_HEIGHT FX(0.3)
#define MAX_CAMERA_PITCH FX(40.0)
#define BEHIND_ORBIT_TIME_CONSTANT FX(0.06)
#define BEHIND_ORIENT_TIME_CONSTANT FX(0.06)
#define FOLLOW_ORBIT_TIME_CONSTANT FX(0.06)
#define FOLLOW_ORIENT_TIME_CONSTANT FX(0.06)
#define MAX_INTERPOLATION_VALUE FX(0.3)
#define BASELINE_INTERPOLATION_SPEED FX(4.5)
#define NO_INTERPOLATION_SPEED FX(2.0)

enum { VIEW_BEHIND = 0, VIEW_FOLLOW = 1, VIEW_ABOVE = 2 };

static const vec3 y_vec = { 0, FX_ONE, 0 };
static const vec3 mz_vec = { 0, 0, -FX_ONE };

static void interpolate_view_pos(vec3 *out, const vec3 *plyr_pos1, const vec3 *plyr_pos2, fx max_vec_angle,
                                 const vec3 *pos1, const vec3 *pos2, fx dist, fx dt, fx time_constant)
{
    vec3 vec1, vec2;
    quat q1, q2;
    fx alpha, theta;
    v3_sub(&vec1, pos1, plyr_pos1);
    v3_sub(&vec2, pos2, plyr_pos2);
    v3_normalize(&vec1);
    v3_normalize(&vec2);
    q_from_vectors(&q1, &y_vec, &vec1);
    q_from_vectors(&q2, &y_vec, &vec2);
    alpha = fminx(MAX_INTERPOLATION_VALUE, fx_one_minus_exp_neg(fxdiv(dt, time_constant)));
    q_nlerp(&q2, &q1, &q2, alpha);
    q_rotate(&vec2, &q2, &y_vec);
    /* constrain angle with x-z plane */
    theta = FX(90.0) - fx_acos_deg(v3_dot(&vec2, &y_vec));
    if (theta > max_vec_angle) {
        vec3 axis, tmp;
        mat34 rot;
        v3_cross(&axis, &y_vec, &vec2);
        v3_normalize(&axis);
        m34_rot_axis(&rot, &axis, theta - max_vec_angle);
        m34_apply_vec(&tmp, &rot, &vec2);
        vec2 = tmp;
    }
    v3_madd(out, plyr_pos2, &vec2, dist);
}

static void interpolate_view_frame(const vec3 *up1, const vec3 *dir1, vec3 *up2, vec3 *dir2, fx dt, fx time_constant)
{
    vec3 x1, y1, z1, x2, y2, z2;
    mat34 m1, m2;
    quat q1, q2;
    fx alpha;
    z1.x = -dir1->x; z1.y = -dir1->y; z1.z = -dir1->z;
    v3_normalize(&z1);
    v3_project_plane(&y1, &z1, up1);
    v3_normalize(&y1);
    v3_cross(&x1, &y1, &z1);
    z2.x = -dir2->x; z2.y = -dir2->y; z2.z = -dir2->z;
    v3_normalize(&z2);
    v3_project_plane(&y2, &z2, up2);
    v3_normalize(&y2);
    v3_cross(&x2, &y2, &z2);
    /* change-of-basis matrices (columns = basis vectors) */
    m1.m[0][0] = x1.x; m1.m[1][0] = x1.y; m1.m[2][0] = x1.z;
    m1.m[0][1] = y1.x; m1.m[1][1] = y1.y; m1.m[2][1] = y1.z;
    m1.m[0][2] = z1.x; m1.m[1][2] = z1.y; m1.m[2][2] = z1.z;
    m2.m[0][0] = x2.x; m2.m[1][0] = x2.y; m2.m[2][0] = x2.z;
    m2.m[0][1] = y2.x; m2.m[1][1] = y2.y; m2.m[2][1] = y2.z;
    m2.m[0][2] = z2.x; m2.m[1][2] = z2.y; m2.m[2][2] = z2.z;
    q_from_mat(&q1, &m1);
    q_from_mat(&q2, &m2);
    alpha = fminx(MAX_INTERPOLATION_VALUE, fx_one_minus_exp_neg(fxdiv(dt, time_constant)));
    q_nlerp(&q2, &q1, &q2, alpha);
    q_to_mat(&m2, &q2);
    up2->x = m2.m[0][1]; up2->y = m2.m[1][1]; up2->z = m2.m[2][1];
    dir2->x = -m2.m[0][2]; dir2->y = -m2.m[1][2]; dir2->z = -m2.m[2][2];
}

void view_update(fx dt)
{
    player_t *p = &g_player;
    vec3 view_pt, view_dir, up_dir, vel_dir, view_vec;
    fx ycoord, course_angle, speed, tcm, a;
    vec3 vel_cpy = p->vel;

    speed = v3_normalize(&vel_cpy);
    {
        fx f = fxdiv(speed - NO_INTERPOLATION_SPEED, BASELINE_INTERPOLATION_SPEED - NO_INTERPOLATION_SPEED);
        f = fclamp(f, 0, FX_ONE);
        tcm = f > FX(0.001) ? fxdiv(FX_ONE, f) : FX(1000.0);
    }
    v3_set(&up_dir, 0, FX_ONE, 0);
    vel_dir = vel_cpy;
    course_angle = g_course.def->angle_deg;
    a = course_angle - CAMERA_ANGLE_ABOVE_SLOPE + PLAYER_ANGLE_IN_CAMERA;
    v3_set(&view_vec, 0, fx_sin_deg(a), fx_cos_deg(a));
    v3_scale(&view_vec, &view_vec, CAMERA_DISTANCE);

    if (p->view_mode == VIEW_BEHIND || p->view_mode == VIEW_FOLLOW) {
        vec3 vel_proj, axis, tmp;
        quat rq;
        mat34 rot;
        int i;
        fx orbit_tc = p->view_mode == VIEW_BEHIND ? BEHIND_ORBIT_TIME_CONSTANT : FOLLOW_ORBIT_TIME_CONSTANT;
        fx orient_tc = p->view_mode == VIEW_BEHIND ? BEHIND_ORIENT_TIME_CONSTANT : FOLLOW_ORIENT_TIME_CONSTANT;
        v3_project_plane(&vel_proj, &y_vec, &vel_dir);
        v3_normalize(&vel_proj);
        q_from_vectors(&rq, &mz_vec, &vel_proj);
        q_rotate(&tmp, &rq, &view_vec);
        view_vec = tmp;
        v3_add(&view_pt, &p->pos, &view_vec);
        ycoord = course_find_y(view_pt.x, view_pt.z);
        if (view_pt.y < ycoord + MIN_CAMERA_HEIGHT) view_pt.y = ycoord + MIN_CAMERA_HEIGHT;
        if (p->view_initialized) {
            for (i = 0; i < 2; i++) {
                vec3 np;
                const vec3 *pp1 = p->view_mode == VIEW_BEHIND ? &p->pos : &p->view_plyr_pos;
                interpolate_view_pos(&np, pp1, &p->pos, MAX_CAMERA_PITCH, &p->view_pos, &view_pt,
                                     CAMERA_DISTANCE, dt, fxmul(orbit_tc, tcm));
                view_pt = np;
            }
        }
        ycoord = course_find_y(view_pt.x, view_pt.z);
        if (view_pt.y < ycoord + ABSOLUTE_MIN_CAMERA_HEIGHT) view_pt.y = ycoord + ABSOLUTE_MIN_CAMERA_HEIGHT;
        v3_sub(&view_vec, &view_pt, &p->pos);
        v3_cross(&axis, &y_vec, &view_vec);
        v3_normalize(&axis);
        m34_rot_axis(&rot, &axis, PLAYER_ANGLE_IN_CAMERA);
        m34_apply_vec(&tmp, &rot, &view_vec);
        view_dir.x = -tmp.x; view_dir.y = -tmp.y; view_dir.z = -tmp.z;
        if (p->view_initialized) {
            for (i = 0; i < 2; i++) {
                interpolate_view_frame(&p->view_up, &p->view_dir, &up_dir, &view_dir, dt, orient_tc);
                v3_set(&up_dir, 0, FX_ONE, 0);
            }
        }
    } else {
        /* ABOVE: camera always uphill of player */
        mat34 rot;
        vec3 tmp;
        v3_add(&view_pt, &p->pos, &view_vec);
        ycoord = course_find_y(view_pt.x, view_pt.z);
        if (view_pt.y < ycoord + MIN_CAMERA_HEIGHT) view_pt.y = ycoord + MIN_CAMERA_HEIGHT;
        v3_sub(&view_vec, &view_pt, &p->pos);
        m34_rot_x(&rot, PLAYER_ANGLE_IN_CAMERA);
        m34_apply_vec(&tmp, &rot, &view_vec);
        view_dir.x = -tmp.x; view_dir.y = -tmp.y; view_dir.z = -tmp.z;
    }
    v3_normalize(&view_dir);
    p->view_pos = view_pt;
    p->view_dir = view_dir;
    p->view_up = up_dir;
    p->view_plyr_pos = p->pos;
    p->view_initialized = 1;
}
