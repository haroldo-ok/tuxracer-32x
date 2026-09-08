/*
 * Tux Racer 32X - game modes (splash/title, course select, intro, racing,
 * game over) - portable, HAL-free.
 */
#ifndef GAME_H
#define GAME_H

#include "fixed.h"
#include "physics.h"

typedef enum {
    MODE_SPLASH = 0,
    MODE_TITLE,
    MODE_COURSE_SELECT,
    MODE_LOADING,
    MODE_INTRO,
    MODE_RACING,
    MODE_GAME_OVER,
    MODE_PAUSED
} game_mode_t;

/* pad bits (same layout as the 32X 68k publishes) */
#define BTN_UP     0x0001
#define BTN_DOWN   0x0002
#define BTN_LEFT   0x0004
#define BTN_RIGHT  0x0008
#define BTN_B      0x0010
#define BTN_C      0x0020
#define BTN_A      0x0040
#define BTN_START  0x0080

typedef struct {
    game_mode_t mode;
    game_mode_t prev_mode;
    int mode_frames;          /* frames since entering the mode */
    int course_index;
    int difficulty;           /* 0 easy 1 normal 2 hard 3 insane */
    int lives;
    int practicing;
    int cup_race;             /* index within the cup (0..2) */
    int race_won;
    int event_mode;           /* 1 = Canadian Cup, 0 = practice */
    int menu_sel;
    u16 pad, pad_prev, pad_pressed;
    /* intro */
    fx intro_time;
    /* results */
    int last_score;
    /* sfx requests (bitmask consumed by platform each frame) */
    int sfx;
    int total_frames;
} game_t;

extern game_t g_gm;
extern int g_frame_counter;

#define SFX_HERRING 1
#define SFX_TREE    2
#define SFX_MENU    4
#define SFX_SNOW    8
#define SFX_ICE     16
#define SFX_ROCK    32
#define SFX_FINISH  64

void game_init(void);
/* one 60 Hz tick: update logic with current pad state */
void game_update(u16 pad);
/* render the current frame into the framebuffer (via render/scene) */
void game_render(void);

#endif
