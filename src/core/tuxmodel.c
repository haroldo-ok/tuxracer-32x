/*
 * Tux Racer 32X - Tux joint animation (port of tux.c adjust_tux_joints)
 * and frame evaluation (equivalent of traverse_dag for the baked model).
 */
#include "tuxmodel.h"

#define MAX_ARM_ANGLE FX(30.0)
#define MAX_PADDLING_ANGLE FX(35.0)
#define MAX_EXT_PADDLING_ANGLE FX(30.0)
#define MAX_KICK_PADDLING_ANGLE FX(20.0)

void tux_pose_reset(tux_pose_t *p)
{
    int i;
    for (i = 0; i < TUX_NUM_FRAMES; i++) m34_identity(&p->joint_rot[i]);
}

/* rotate_scene_node(node, axis, angle): node.trans = node.trans * R */
static void joint_rotate(tux_pose_t *p, int frame, char axis, fx deg)
{
    mat34 r, tmp;
    if (deg == 0) return;
    if (axis == 'x') m34_rot_x(&r, deg);
    else if (axis == 'y') m34_rot_y(&r, deg);
    else m34_rot_z(&r, deg);
    m34_mul(&tmp, &p->joint_rot[frame], &r);
    p->joint_rot[frame] = tmp;
}

/* sin of (factor * pi) etc: factor in 16.16 turns -> degrees */
static fx sin_pi_times(fx f)          /* sin(f * pi) */
{
    return fx_sin_deg(fxmul(f, FX(180.0)));
}

void tux_pose_racing(tux_pose_t *p, fx turn_fact, int is_braking, fx paddling_factor,
                     fx speed, const vec3 *local_force, fx flap_factor)
{
    fx turning_angle[2];
    fx paddling_angle, ext_paddling_angle, kick_paddling_angle, braking_angle = 0;
    fx force_angle, turn_leg_angle, flap_angle;
    fx spd35 = fminx(FX(35.0), speed), spd50 = fminx(FX(50.0), speed);

    tux_pose_reset(p);

    if (is_braking) braking_angle = MAX_ARM_ANGLE;
    paddling_angle = fxmul(MAX_PADDLING_ANGLE, sin_pi_times(paddling_factor));
    ext_paddling_angle = fxmul(MAX_EXT_PADDLING_ANGLE, sin_pi_times(paddling_factor));
    kick_paddling_angle = fxmul(MAX_KICK_PADDLING_ANGLE, sin_pi_times(paddling_factor * 2));
    turning_angle[0] = fxmul(fmaxx(-turn_fact, 0), MAX_ARM_ANGLE);
    turning_angle[1] = fxmul(fmaxx(turn_fact, 0), MAX_ARM_ANGLE);
    /* flap_angle = MAX_ARM_ANGLE * (0.5 + 0.5*sin(pi*flap*6 - pi/2)) */
    flap_angle = fxmul(MAX_ARM_ANGLE, FX_HALF + (fx_sin_deg(fxmul(flap_factor, FX(1080.0)) - FX(90.0)) >> 1));

    joint_rotate(p, TUX_FRAME_LSHOULDER, 'z', fminx(braking_angle + paddling_angle + turning_angle[0], MAX_ARM_ANGLE) + flap_angle);
    joint_rotate(p, TUX_FRAME_RSHOULDER, 'z', fminx(braking_angle + paddling_angle + turning_angle[1], MAX_ARM_ANGLE) + flap_angle);
    joint_rotate(p, TUX_FRAME_LSHOULDER, 'y', -ext_paddling_angle);
    joint_rotate(p, TUX_FRAME_RSHOULDER, 'y', ext_paddling_angle);

    force_angle = fclamp(fxdiv(-local_force->z, FX(300.0)), FX(-20.0), FX(20.0));
    turn_leg_angle = turn_fact * 10;

    joint_rotate(p, TUX_FRAME_LHIP, 'z', FX(-20.0) + turn_leg_angle + force_angle);
    joint_rotate(p, TUX_FRAME_RHIP, 'z', FX(-20.0) - turn_leg_angle + force_angle);

    joint_rotate(p, TUX_FRAME_LKNEE, 'z', FX(-10.0) + turn_leg_angle - spd35 + kick_paddling_angle + force_angle);
    joint_rotate(p, TUX_FRAME_RKNEE, 'z', FX(-10.0) - turn_leg_angle - spd35 - kick_paddling_angle + force_angle);

    joint_rotate(p, TUX_FRAME_LANKLE, 'z', FX(-20.0) + spd50);
    joint_rotate(p, TUX_FRAME_RANKLE, 'z', FX(-20.0) + spd50);

    joint_rotate(p, TUX_FRAME_TAIL, 'z', turn_fact * 20);

    joint_rotate(p, TUX_FRAME_NECK, 'z', FX(-50.0));
    joint_rotate(p, TUX_FRAME_HEAD, 'z', FX(-30.0));
    joint_rotate(p, TUX_FRAME_HEAD, 'y', -turn_fact * 70);
}

void tux_pose_keyframe(tux_pose_t *p, fx l_shldr, fx r_shldr, fx l_hip, fx r_hip)
{
    tux_pose_reset(p);
    joint_rotate(p, TUX_FRAME_LSHOULDER, 'z', l_shldr);
    joint_rotate(p, TUX_FRAME_RSHOULDER, 'z', r_shldr);
    joint_rotate(p, TUX_FRAME_LHIP, 'z', l_hip);
    joint_rotate(p, TUX_FRAME_RHIP, 'z', r_hip);
}

void tux_eval_frames(const tux_pose_t *p, const mat34 *root, mat34 out[TUX_NUM_FRAMES])
{
    int i;
    mat34 tmp;
    /* frame 0 (body) = root * r1 (which carries the 0.35 scale) */
    m34_mul(&out[0], root, &tux_frame_defs[0].local);
    for (i = 1; i < TUX_NUM_FRAMES; i++) {
        const tux_frame_def_t *fd = &tux_frame_defs[i];
        /* world = parent * local * joint_rot */
        m34_mul(&tmp, &out[fd->parent], &fd->local);
        m34_mul(&out[i], &tmp, &p->joint_rot[i]);
    }
}
