/*
 * Tux Racer 32X - Tux hierarchical model (baked from tux.tcl).
 *
 * The original model is a DAG of transform nodes with unit spheres as
 * leaves; joints (shoulders, hips, knees, ankles, neck, head, tail) are
 * transform nodes that tux.c rotates every frame.  We bake every sphere
 * into an ellipsoid expressed in the frame of its nearest ancestor joint:
 *   world = Frame[k] * (centre + ax0*u + ax1*v + ax2*w)
 * and evaluate the frames at runtime exactly like traverse_dag would.
 */
#ifndef TUXMODEL_H
#define TUXMODEL_H

#include "fixed.h"

enum {
    TUX_FRAME_BODY = 0,
    TUX_FRAME_NECK, TUX_FRAME_HEAD,
    TUX_FRAME_LSHOULDER, TUX_FRAME_RSHOULDER,
    TUX_FRAME_LHIP, TUX_FRAME_RHIP,
    TUX_FRAME_LKNEE, TUX_FRAME_RKNEE,
    TUX_FRAME_LANKLE, TUX_FRAME_RANKLE,
    TUX_FRAME_TAIL,
    TUX_NUM_FRAMES
};

enum { TUX_MAT_WHITE = 0, TUX_MAT_BLACK, TUX_MAT_BEAK, TUX_MAT_NOSTRIL, TUX_MAT_IRIS };

typedef struct {
    int parent;        /* parent frame index or -1 */
    mat34 local;       /* transform from parent frame to this joint (before joint rotation) */
} tux_frame_def_t;

typedef struct {
    u16 i0, i1, i2;       /* vertex indices (per frame vertex list) */
    u8 material;
    s8 n[3];              /* flat normal * 127, in frame-local space */
} tux_tri_t;

typedef struct {
    const vec3 *verts;    /* frame-local positions (16.16) */
    int num_verts;
    const tux_tri_t *tris;
    int num_tris;
} tux_mesh_t;

extern const tux_frame_def_t tux_frame_defs[];
extern const int tux_num_frames;
extern const tux_mesh_t tux_mesh_lod0[TUX_NUM_FRAMES];   /* near: ~500 tris */
extern const tux_mesh_t tux_mesh_lod1[TUX_NUM_FRAMES];   /* far: ~200 tris */

/* per-frame joint pose (rotation applied at the joint) */
typedef struct {
    mat34 joint_rot[TUX_NUM_FRAMES];   /* rotation applied at joint (identity by default) */
} tux_pose_t;

void tux_pose_reset(tux_pose_t *p);
/* tux.c adjust_tux_joints: all angles are 16.16 degrees / factors */
void tux_pose_racing(tux_pose_t *p, fx turn_fact, int is_braking, fx paddling_factor,
                     fx speed, const vec3 *local_force, fx flap_factor);
/* keyframe pose (intro walk): shoulders/hips only */
void tux_pose_keyframe(tux_pose_t *p, fx l_shldr, fx r_shldr, fx l_hip, fx r_hip);
/* evaluate world matrices for all frames given root (position+orientation) */
void tux_eval_frames(const tux_pose_t *p, const mat34 *root, mat34 out[TUX_NUM_FRAMES]);

#endif
