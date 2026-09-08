/*
 * Tux Racer 32X - palette construction.
 */
#include "palette.h"

const vec3 g_light_dir = { FX(0.70710678), FX(0.70710678), 0 };

static u16 cram(int r, int g, int b)   /* 0..255 each */
{
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (u16)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

/* material ramp: colour = base * (ambient + diffuse * level/15) per channel */
static void ramp(u16 *out, int base, int r, int g, int b, int amb_r, int amb_g, int amb_b)
{
    int i;
    for (i = 0; i < RAMP_LEVELS; i++) {
        /* ambient (.45,.53,.75) + diffuse (1,.9,1) * level */
        int lr = amb_r + (100 * i) / 15;
        int lg = amb_g + (90 * i) / 15;
        int lb = amb_b + (100 * i) / 15;
        out[base + i] = cram((r * lr) / 100, (g * lg) / 100, (b * lb) / 100);
    }
}

void palette_build(u16 out[256])
{
    int i;
    for (i = 0; i < 256; i++) out[i] = 0;
    out[PAL_BLACK] = cram(0, 0, 0);
    /* sky gradient: Tux Racer sunny sky is a light blue; top darker */
    for (i = 0; i < 8; i++) {
        out[PAL_SKY + i] = cram(90 + i * 14, 140 + i * 11, 220 + i * 4);
    }
    ramp(out, PAL_SNOW, 240, 244, 255, 45, 53, 75);
    ramp(out, PAL_ICE, 170, 200, 240, 45, 53, 75);
    ramp(out, PAL_ROCK, 120, 105, 95, 45, 53, 75);
    ramp(out, PAL_TREE_GREEN, 30, 110, 40, 45, 53, 75);
    ramp(out, PAL_TRUNK, 100, 65, 35, 45, 53, 75);
    ramp(out, PAL_TUX_BLACK, 40, 40, 48, 45, 53, 75);
    ramp(out, PAL_TUX_WHITE, 235, 235, 240, 45, 53, 75);
    ramp(out, PAL_BEAK, 225, 165, 30, 45, 53, 75);
    ramp(out, PAL_HERRING, 60, 190, 210, 45, 53, 75);
    ramp(out, PAL_FLAG, 200, 40, 40, 45, 53, 75);
    ramp(out, PAL_TREE_BARREN, 170, 120, 70, 45, 53, 75);
    for (i = 0; i < 8; i++) {
        /* finish banner: alternating red/white checker ramp */
        out[PAL_FINISH + i] = (i & 1) ? cram(230, 230, 230) : cram(200, 30, 30);
    }
    out[PAL_UI_WHITE] = cram(255, 255, 255);
    out[PAL_UI_YELLOW] = cram(255, 230, 60);
    out[PAL_UI_RED] = cram(230, 50, 50);
    out[PAL_UI_GREY] = cram(160, 160, 170);
    out[PAL_UI_DKGREY] = cram(70, 70, 80);
    out[PAL_UI_BLUE] = cram(40, 80, 200);
    out[PAL_UI_ORANGE] = cram(255, 150, 40);
    out[PAL_UI_GREEN] = cram(60, 200, 80);
    out[PAL_UI_SHADOW] = cram(20, 20, 40);
    out[PAL_UI_PANEL] = cram(30, 50, 110);
    out[PAL_HERRING_ICON] = cram(80, 200, 220);
    out[PAL_HERRING_ICON + 1] = cram(255, 120, 60);
    for (i = 0; i < 4; i++) out[PAL_SNOW_FOG + i] = cram(200 + i * 10, 215 + i * 8, 245 + i * 2);
    out[PAL_TUX_EYE] = cram(250, 250, 250);
    out[PAL_TUX_EYE + 1] = cram(10, 10, 10);
}
