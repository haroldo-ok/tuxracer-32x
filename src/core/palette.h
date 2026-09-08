/*
 * Tux Racer 32X - palette layout.  Each material is a 16-entry light ramp:
 * index = base + level, level 0 = fully shadowed, 15 = fully lit.
 * Colours follow the Tux Racer materials / textures (sunny_light.tcl:
 * light dir (1,1,0), diffuse (1,.9,1), ambient (.45,.53,.75)).
 */
#ifndef PALETTE_H
#define PALETTE_H

#include "fixed.h"

#define PAL_BLACK       0
#define PAL_SKY         1      /* 1..8  : sky gradient (top -> horizon) */
#define RAMP_LEVELS     16
#define PAL_SNOW        16
#define PAL_ICE         32
#define PAL_ROCK        48
#define PAL_TREE_GREEN  64
#define PAL_TRUNK       80
#define PAL_TUX_BLACK   96
#define PAL_TUX_WHITE   112
#define PAL_BEAK        128
#define PAL_HERRING     144
#define PAL_FLAG        160
#define PAL_TREE_BARREN 176
#define PAL_FINISH      192    /* 192..199 finish/start banner ramp (8) */
#define PAL_UI_WHITE    200
#define PAL_UI_YELLOW   201
#define PAL_UI_RED      202
#define PAL_UI_GREY     203
#define PAL_UI_DKGREY   204
#define PAL_UI_BLUE     205
#define PAL_UI_ORANGE   206
#define PAL_UI_GREEN    207
#define PAL_UI_SHADOW   208
#define PAL_UI_PANEL    209
#define PAL_HERRING_ICON 210
#define PAL_SNOW_FOG    212    /* 212..215 fogged snow (4 far levels) */
#define PAL_TUX_EYE     216
#define PAL_NUM_USED    220

/* build the 256-entry palette as 32X CRAM words: 0bBBBBBGGGGGRRRRR */
void palette_build(u16 out[256]);

/* light direction (unit) used by the shading code */
extern const vec3 g_light_dir;

/* shade level 0..15 from a unit normal (world space) */
static inline int shade_level(const vec3 *n)
{
    /* dot with (0.7071, 0.7071, 0); ambient floor */
    fx d = fxmul(n->x + n->y, FX(0.70710678));
    if (d < 0) d = 0;
    return (d * 15 + FX_HALF) >> FX_SHIFT;
}

#endif
