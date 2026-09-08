/*
 * Tux Racer 32X - scene drawing (terrain, trees, items, Tux, sky).
 */
#ifndef SCENE_H
#define SCENE_H

#include "fixed.h"
#include "tuxmodel.h"

/* draw the sky/background into the framebuffer (fills entire screen) */
void scene_draw_sky(void);
/* queue the visible terrain around world z (course_render.c equivalent) */
void scene_draw_terrain(fx cam_z, fx cam_x);
/* queue trees and items in range */
void scene_draw_objects(fx cam_z);
/* queue Tux: root matrix = translation(pos) * orientation */
void scene_draw_tux(const mat34 *root, const tux_pose_t *pose);
/* level-of-detail control (set from platform depending on budget) */
extern int g_scene_lod;       /* 0 = full, 1 = reduced */
extern int g_scene_tri_budget;

#endif
