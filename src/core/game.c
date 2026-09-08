/*
 * Tux Racer 32X - game flow.  Follows the original mode structure
 * (splash_screen -> game_type_select -> race_select -> loading -> intro
 * -> racing -> game_over), with the menus reduced to what a gamepad can
 * drive.  Scoring, requirements and lives follow race_select.c /
 * game_over.c / course_idx.tcl.
 */
#include "game.h"
#include "render.h"
#include "scene.h"
#include "palette.h"
#include "tuxmodel.h"

game_t g_gm;
int g_frame_counter;
extern int g_herring_pickup_event;

/* ---- intro key frames (courses/common/tux_walk.tcl) ------------------- */
typedef struct { fx time, x, z, y, yaw, pitch, l_shldr, r_shldr, l_hip, r_hip; } keyframe_t;
static const keyframe_t keyframes[] = {
    { FX(0.5), FX(-1.05), FX(-1.0), FX(0.455), FX(90), 0, FX(-20.6), FX(39.8), FX(-23.4), FX(18.2) },
    { FX(1.0), FX(-0.7), FX(-1.0), FX(0.455), FX(90), 0, FX(39.8), FX(-20.6), FX(18.2), FX(-23.4) },
    { FX(1.5), FX(-0.35), FX(-1.0), FX(0.455), FX(90), 0, FX(-20.6), FX(39.8), FX(-23.4), FX(18.2) },
    { FX(2.0), 0, FX(-1.0), FX(0.455), FX(90), 0, FX(39.8), FX(-20.6), FX(18.2), FX(-23.4) },
    { FX(2.5), 0, FX(-1.0), FX(0.455), FX(180), 0, 0, 0, 0, 0 },
    { FX(3.0), 0, FX(-1.0), FX(0.455), FX(180), 0, FX(-60), FX(-60), 0, 0 },
    { FX(3.2), 0, FX(-1.0), FX(0.455), FX(180), 0, FX(20), FX(20), 0, 0 },
    { FX(3.6), 0, FX(-0.3), FX(0.8), FX(180), FX(60), FX(90), FX(90), FX(-20), FX(-20) },
    { FX(4.0), 0, 0, 0, FX(180), FX(120), 0, 0, FX(-40), FX(-40) },
};
#define NUM_KEYFRAMES ((int)(sizeof(keyframes) / sizeof(keyframes[0])))

static tux_pose_t g_pose;
static mat34 g_tux_root;

static const char *difficulty_names[4] = { "Easy", "Normal", "Hard", "Insane" };

static void set_mode(game_mode_t m)
{
    g_gm.prev_mode = g_gm.mode;
    g_gm.mode = m;
    g_gm.mode_frames = 0;
}

void game_init(void)
{
    int i;
    for (i = 0; i < (int)sizeof(g_gm); i++) ((u8 *)&g_gm)[i] = 0;
    g_gm.mode = MODE_SPLASH;
    g_gm.difficulty = 1;
    g_gm.lives = 4;                 /* INIT_NUM_LIVES */
    g_gm.event_mode = 1;
    g_gm.menu_sel = 0;
    g_frame_counter = 0;
    tux_pose_reset(&g_pose);
    m34_identity(&g_tux_root);
}

/* ------------------------------------------------------------ intro */

static void intro_start(void)
{
    course_load(g_gm.course_index);
    g_game.time = 0;
    g_game.race_over = 0;
    g_game.race_aborted = 0;
    g_player.herring = 0;
    g_player.score = 0;
    g_player.orientation_initialized = 0;
    g_player.view_initialized = 0;
    physics_init();
    v3_set(&g_player.vel, 0, 0, 0);
    g_player.view_mode = 2;         /* ABOVE during intro (intro.c) */
    view_update(FX(0.0001));
    g_gm.intro_time = 0;
}

static void racing_start(void)
{
    g_player.view_mode = 1;         /* FOLLOW: Tux Racer's default view_mode */
    physics_init();
    g_game.time = 0;
    g_game.race_over = 0;
    g_game.race_aborted = 0;
    g_player.herring = 0;
    g_player.view_initialized = 0;
    view_update(FX(0.0001));
}

static void update_key_frame(fx t)
{
    int idx;
    fx frac;
    const keyframe_t *a, *b;
    vec3 pos;
    mat34 ry, rx, cob;
    fx v;
    for (idx = 0; idx < NUM_KEYFRAMES; idx++) if (t < keyframes[idx].time) break;
    if (idx >= NUM_KEYFRAMES) { g_gm.intro_time = FX(99.0); return; }
    if (idx == 0) { a = &keyframes[0]; b = &keyframes[0]; frac = FX_ONE; }
    else {
        a = &keyframes[idx - 1]; b = &keyframes[idx];
        if (fabsx(a->time - b->time) < 10) frac = FX_ONE;
        else frac = fxdiv(t - b->time, a->time - b->time);
    }
#define INTERP(f) (b->f + fxmul(frac, a->f - b->f))
    pos.x = INTERP(x) + g_course.def->start_x;
    pos.z = -INTERP(z) + g_course.def->start_z;
    pos.y = INTERP(y) + course_find_y(pos.x, pos.z);
    player_set_pos(&pos);
    v = INTERP(yaw);
    m34_rot_y(&ry, v);
    v = INTERP(pitch);
    m34_rot_x(&rx, v);
    m34_mul(&cob, &ry, &rx);
    q_from_mat(&g_player.orientation, &cob);
    g_player.orientation_initialized = 1;
    tux_pose_keyframe(&g_pose, INTERP(l_shldr), INTERP(r_shldr), INTERP(l_hip), INTERP(r_hip));
#undef INTERP
}

/* ------------------------------------------------------------ results */

static void compute_result(void)
{
    const course_def_t *d = g_course.def;
    fx par = d->par_time;
    int score;
    /* game_over.c: score = max(0, 100*(par - time) + 200*herring)  (par = easy time req) */
    fx dt = par - g_game.time;
    score = FX_INT(dt * 100) + 200 * g_player.herring;
    if (score < 0) score = 0;
    g_player.score = score;
    g_gm.last_score = score;
    g_gm.race_won = (g_game.time <= d->time_req[g_gm.difficulty] && g_player.herring >= d->herring_req[g_gm.difficulty]);
    if (g_gm.event_mode && !g_gm.race_won) {
        g_gm.lives--;
    }
}

/* ------------------------------------------------------------ update */

void game_update(u16 pad)
{
    fx dt = FX(1.0 / 60.0);
    g_gm.pad_prev = g_gm.pad;
    g_gm.pad = pad;
    g_gm.pad_pressed = (u16)(pad & ~g_gm.pad_prev);
    g_gm.mode_frames++;
    g_gm.total_frames++;
    g_frame_counter++;
    g_gm.sfx = 0;

    switch (g_gm.mode) {
    case MODE_SPLASH:
        if (g_gm.mode_frames > 150 || (g_gm.pad_pressed & (BTN_START | BTN_A | BTN_B | BTN_C))) {
            set_mode(MODE_TITLE);
        }
        break;

    case MODE_TITLE:
        /* game_type_select: Enter an event / Practice / (difficulty) */
        if (g_gm.pad_pressed & BTN_UP) { g_gm.menu_sel = (g_gm.menu_sel + 2) % 3; g_gm.sfx |= SFX_MENU; }
        if (g_gm.pad_pressed & BTN_DOWN) { g_gm.menu_sel = (g_gm.menu_sel + 1) % 3; g_gm.sfx |= SFX_MENU; }
        if (g_gm.menu_sel == 2) {
            if (g_gm.pad_pressed & BTN_LEFT) { g_gm.difficulty = (g_gm.difficulty + 3) % 4; g_gm.sfx |= SFX_MENU; }
            if (g_gm.pad_pressed & BTN_RIGHT) { g_gm.difficulty = (g_gm.difficulty + 1) % 4; g_gm.sfx |= SFX_MENU; }
        }
        if (g_gm.pad_pressed & (BTN_START | BTN_A | BTN_B | BTN_C)) {
            g_gm.sfx |= SFX_MENU;
            if (g_gm.menu_sel == 0) {
                g_gm.event_mode = 1;
                g_gm.practicing = 0;
                g_gm.lives = 4;
                g_gm.cup_race = 0;
                g_gm.course_index = 0;
                set_mode(MODE_COURSE_SELECT);
            } else if (g_gm.menu_sel == 1) {
                g_gm.event_mode = 0;
                g_gm.practicing = 1;
                g_gm.course_index = 0;
                set_mode(MODE_COURSE_SELECT);
            } else {
                g_gm.difficulty = (g_gm.difficulty + 1) % 4;
            }
        }
        break;

    case MODE_COURSE_SELECT: {
        int n = g_gm.event_mode ? 3 : num_course_defs;
        if (g_gm.event_mode) {
            /* in the cup the current race is fixed: cup_race */
            g_gm.course_index = g_gm.cup_race;
        } else {
            if (g_gm.pad_pressed & (BTN_UP | BTN_LEFT)) { g_gm.course_index = (g_gm.course_index + n - 1) % n; g_gm.sfx |= SFX_MENU; }
            if (g_gm.pad_pressed & (BTN_DOWN | BTN_RIGHT)) { g_gm.course_index = (g_gm.course_index + 1) % n; g_gm.sfx |= SFX_MENU; }
        }
        if (g_gm.pad_pressed & (BTN_START | BTN_A | BTN_B | BTN_C)) {
            g_gm.sfx |= SFX_MENU;
            set_mode(MODE_LOADING);
        }
        break;
    }

    case MODE_LOADING:
        if (g_gm.mode_frames == 2) {
            intro_start();
            set_mode(MODE_INTRO);
        }
        break;

    case MODE_INTRO:
        g_gm.intro_time += dt;
        update_key_frame(g_gm.intro_time);
        view_update(dt);
        if (g_gm.intro_time >= FX(4.0) || (g_gm.pad_pressed & (BTN_START | BTN_A | BTN_B | BTN_C))) {
            racing_start();
            set_mode(MODE_RACING);
        }
        break;

    case MODE_RACING: {
        input_t in;
        int terr_mask;
        in.left = (pad & BTN_LEFT) != 0;
        in.right = (pad & BTN_RIGHT) != 0;
        in.paddle = (pad & BTN_UP) != 0;
        in.brake = (pad & BTN_DOWN) != 0;
        in.charge = (pad & (BTN_A | BTN_B | BTN_C)) != 0;
        g_herring_pickup_event = 0;
        g_player.collision = 0;
        racing_update(&in, dt);
        view_update(dt);
        if (g_herring_pickup_event) g_gm.sfx |= SFX_HERRING;
        if (g_player.collision) g_gm.sfx |= SFX_TREE;
        terr_mask = g_player.last_terrain;
        if (terr_mask & (1 << TERRAIN_SNOW)) g_gm.sfx |= SFX_SNOW;
        else if (terr_mask & (1 << TERRAIN_ICE)) g_gm.sfx |= SFX_ICE;
        else if (terr_mask & (1 << TERRAIN_ROCK)) g_gm.sfx |= SFX_ROCK;
        /* pose for rendering */
        tux_pose_racing(&g_pose, g_player.control.turn_animation, g_player.control.is_braking,
                        g_player.anim_paddling_factor, g_player.anim_speed, &g_player.anim_local_force,
                        g_player.anim_flap_factor);
        if (g_gm.pad_pressed & BTN_START) {
            set_mode(MODE_PAUSED);
        }
        if (g_game.race_over) {
            compute_result();
            g_gm.sfx |= SFX_FINISH;
            set_mode(MODE_GAME_OVER);
        }
        break;
    }

    case MODE_PAUSED:
        if (g_gm.pad_pressed & BTN_START) set_mode(MODE_RACING);
        if (g_gm.pad_pressed & BTN_A) {   /* abort race */
            g_game.race_aborted = 1;
            g_game.race_over = 1;
            g_player.score = 0;
            g_gm.race_won = 0;
            if (g_gm.event_mode) g_gm.lives--;
            set_mode(MODE_GAME_OVER);
        }
        break;

    case MODE_GAME_OVER:
        if (g_gm.mode_frames > 30 && (g_gm.pad_pressed & (BTN_START | BTN_A | BTN_B | BTN_C))) {
            g_gm.sfx |= SFX_MENU;
            if (g_gm.event_mode) {
                if (g_gm.race_won) {
                    if (g_gm.cup_race >= 2) {
                        set_mode(MODE_TITLE);      /* cup complete */
                    } else {
                        g_gm.cup_race++;
                        set_mode(MODE_COURSE_SELECT);
                    }
                } else if (g_gm.lives <= 0) {
                    set_mode(MODE_TITLE);
                } else {
                    set_mode(MODE_COURSE_SELECT);
                }
            } else {
                set_mode(MODE_COURSE_SELECT);
            }
        }
        break;
    }
}

/* ------------------------------------------------------------ render */

static void draw_text_shadow(int x, int y, const char *s, u8 color)
{
    draw_text(x + 1, y + 1, s, PAL_UI_SHADOW);
    draw_text(x, y, s, color);
}

static void draw_text_big_shadow(int x, int y, const char *s, u8 color)
{
    draw_text_big(x + 2, y + 2, s, PAL_UI_SHADOW);
    draw_text_big(x, y, s, color);
}

static void format_time(char *buf, fx t)
{
    int total_h = FX_INT(t * 100 + FX_HALF);     /* hundredths */
    int minutes, seconds, hundredths;
    if (total_h < 0) total_h = 0;
    minutes = total_h / 6000;
    seconds = (total_h / 100) % 60;
    hundredths = total_h % 100;
    buf[0] = (char)('0' + (minutes / 10) % 10);
    buf[1] = (char)('0' + minutes % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + seconds / 10);
    buf[4] = (char)('0' + seconds % 10);
    buf[5] = '.';
    buf[6] = (char)('0' + hundredths / 10);
    buf[7] = (char)('0' + hundredths % 10);
    buf[8] = 0;
}

static void itoa_pad(char *buf, int v, int width)
{
    int i;
    buf[width] = 0;
    for (i = width - 1; i >= 0; i--) {
        if (v > 0 || i == width - 1) { buf[i] = (char)('0' + v % 10); v /= 10; }
        else buf[i] = ' ';
    }
}

/* hud.c: time top-left, herring count top-right, speed/energy gauge bottom-right */
static void draw_hud(void)
{
    char buf[16];
    int speed_kmh;
    /* time */
    draw_text_shadow(8, 6, "Time:", PAL_UI_WHITE);
    format_time(buf, g_game.time);
    draw_text_big_shadow(8, 16, buf, PAL_UI_YELLOW);
    /* herring */
    itoa_pad(buf, g_player.herring, 3);
    draw_text_big_shadow(SCREEN_W - 8 - 16 * 3, 16, buf, PAL_UI_YELLOW);
    /* herring icon: a small fish */
    fill_tri(SCREEN_W - 24, 8, SCREEN_W - 12, 4, SCREEN_W - 12, 12, PAL_HERRING_ICON);
    fill_tri(SCREEN_W - 12, 8, SCREEN_W - 6, 4, SCREEN_W - 6, 12, PAL_HERRING_ICON + 1);
    /* speed gauge (km/h) */
    speed_kmh = FX_INT(fxmul(g_player.anim_speed, FX(3.6)) + FX_HALF);
    {
        int i, bars = speed_kmh / 5;
        if (bars > 20) bars = 20;
        for (i = 0; i < 20; i++) {
            u8 c = i < bars ? (i < 10 ? PAL_UI_GREEN : (i < 16 ? PAL_UI_YELLOW : PAL_UI_RED)) : PAL_UI_DKGREY;
            fill_rect(SCREEN_W - 14 - (19 - i) * 5, SCREEN_H - 24 - i, 4, 4 + i, c);
        }
        itoa_pad(buf, speed_kmh, 3);
        draw_text_shadow(SCREEN_W - 12 - 8 * 8, SCREEN_H - 16, buf, PAL_UI_WHITE);
        draw_text_shadow(SCREEN_W - 12 - 8 * 4, SCREEN_H - 16, "km/h", PAL_UI_WHITE);
    }
    /* energy/jump charge bar */
    if (g_player.control.jump_charging || g_player.control.jumping) {
        int w = FX_INT(g_player.control.jump_amt * 60);
        fill_rect(12, SCREEN_H - 20, 62, 8, PAL_UI_DKGREY);
        fill_rect(13, SCREEN_H - 19, w, 6, PAL_UI_ORANGE);
    }
    /* lives in event mode */
    if (g_gm.event_mode) {
        int i;
        for (i = 0; i < g_gm.lives; i++) fill_rect(12 + i * 10, SCREEN_H - 32, 6, 6, PAL_UI_WHITE);
    }
}

extern int g_prof_skip;
#ifdef DEBUG_OVERLAY
extern unsigned g_prof_stage[8];
/* PicoDrive has no FRT, so stages are timed with the 68k vblank counter
   (COMM10) and accumulated across frames; tools/profile.py takes the
   difference between two screenshots.  PROF_REP > 1 repeats each stage
   (the triangle queue is rewound between repeats). */
#ifndef PROF_REP
#define PROF_REP 1
#endif
static inline unsigned prof_now(void) { return *(volatile u16 *)0x2000402A; }
#define PROF_BEGIN(i) { unsigned pt = prof_now(); int rep; int n0 = g_rs->num_tris; \
                        for (rep = 0; rep < PROF_REP; rep++) { if (rep) g_rs->num_tris = n0;
#define PROF_END(i)   } g_prof_stage[i] += (prof_now() - pt) & 0xffff; }
#else
#define PROF_BEGIN(i) {
#define PROF_END(i)   }
#endif
static void draw_world(void)
{
    vec3 dir = g_player.view_dir, up = g_player.view_up;
    render_set_camera(&g_player.view_pos, &dir, &up);
    PROF_BEGIN(0)
    if (!(g_prof_skip & 8)) scene_draw_sky();
    PROF_END(0)
    /* pass 1: terrain (depth sorted amongst itself) */
    PROF_BEGIN(1)
    render_begin_frame();
    if (!(g_prof_skip & 1)) scene_draw_terrain(g_player.view_pos.z, g_player.view_pos.x);
    PROF_END(1)
    PROF_BEGIN(2)
    if (!(g_prof_skip & 4)) render_flush();
    PROF_END(2)
    /* pass 2: trees, items and Tux sorted together, always over the terrain */
    if (!(g_prof_skip & 2)) {
        PROF_BEGIN(3)
        render_begin_frame();
        scene_draw_objects(g_player.view_pos.z);
        PROF_END(3)
        PROF_BEGIN(4)
        q_to_mat(&g_tux_root, &g_player.orientation);
        g_tux_root.t = g_player.pos;
        g_tux_root.t.y += FX(0.33);     /* TUX_Y_CORRECTION_ON_STOMACH (set_tux_pos) */
        scene_draw_tux(&g_tux_root, &g_pose);
        PROF_END(4)
    }
    PROF_BEGIN(5)
    if (!(g_prof_skip & 4)) render_flush();
    PROF_END(5)
}

static void draw_panel(int x, int y, int w, int h)
{
    fill_rect(x + 3, y + 3, w, h, PAL_UI_SHADOW);
    fill_rect(x, y, w, h, PAL_UI_PANEL);
    fill_rect(x, y, w, 1, PAL_UI_WHITE);
    fill_rect(x, y + h - 1, w, 1, PAL_UI_WHITE);
}

static void draw_title_backdrop(void)
{
    /* sky + a snowy mountain silhouette drawn with real polygons */
    scene_draw_sky();
    fill_tri(-10, 200, 90, 70, 190, 200, PAL_SNOW + 13);
    fill_tri(120, 200, 230, 40, 340, 200, PAL_SNOW + 15);
    fill_tri(120, 200, 230, 40, 230, 200, PAL_SNOW + 9);
    fill_tri(-10, 200, 90, 70, 90, 200, PAL_SNOW + 8);
    fill_rect(0, 196, SCREEN_W, SCREEN_H - 196, PAL_SNOW + 14);
    /* a few trees */
    fill_tri(40, 200, 52, 170, 64, 200, PAL_TREE_GREEN + 9);
    fill_tri(270, 200, 284, 160, 298, 200, PAL_TREE_GREEN + 11);
    fill_tri(300, 200, 308, 178, 316, 200, PAL_TREE_GREEN + 7);
}

static void draw_title_tux(void)
{
    /* Tux standing on the title screen, slowly turning (real 3D model) */
    vec3 pos = { 0, 0, 0 }, dir = { 0, FX(-0.10), -FX_ONE }, up = { 0, FX_ONE, 0 };
    vec3 campos = { FX(-0.55), FX(0.25), FX(1.35) };
    mat34 root;
    tux_pose_t pose;
    vec3 lf = { 0, 0, 0 };
    v3_normalize(&dir);
    render_set_camera(&campos, &dir, &up);
    render_begin_frame();
    m34_rot_y(&root, FX_FROM_INT((g_frame_counter * 1) % 360) + FX(180.0));
    root.t = pos;
    tux_pose_racing(&pose, 0, 0, 0, FX(0.0), &lf, 0);
    scene_draw_tux(&root, &pose);
    render_flush();
}

void game_render(void)
{
    char buf[32];
    switch (g_gm.mode) {
    case MODE_SPLASH:
        fill_rect(0, 0, SCREEN_W, SCREEN_H, PAL_BLACK);
        draw_text_big_shadow((SCREEN_W - 16 * 9) / 2, 80, "TUX RACER", PAL_UI_WHITE);
        draw_text_shadow((SCREEN_W - 8 * 20) / 2, 110, "Sega 32X edition 0.61", PAL_UI_GREY);
        draw_text_shadow((SCREEN_W - 8 * 30) / 2, 140, "(c) 1999-2001 Jasmin F. Patry", PAL_UI_GREY);
        draw_text_shadow((SCREEN_W - 8 * 22) / 2, 152, "GPL - www.tuxracer.com", PAL_UI_GREY);
        break;

    case MODE_TITLE: {
        int i;
        static const char *items[3] = { "Enter an event", "Practice", "Difficulty:" };
        draw_title_backdrop();
        draw_title_tux();
        draw_text_big_shadow((SCREEN_W - 16 * 9) / 2, 18, "TUX RACER", PAL_UI_YELLOW);
        draw_panel(20, 96, 150, 70);
        for (i = 0; i < 3; i++) {
            u8 c = i == g_gm.menu_sel ? PAL_UI_YELLOW : PAL_UI_WHITE;
            draw_text_shadow(36, 106 + i * 18, items[i], c);
            if (i == g_gm.menu_sel) draw_text_shadow(24, 106 + i * 18, ">", PAL_UI_YELLOW);
        }
        draw_text_shadow(36 + 8 * 12, 106 + 2 * 18, difficulty_names[g_gm.difficulty], PAL_UI_ORANGE);
        draw_text_shadow(8, SCREEN_H - 12, "D-pad: choose   Start/A/B/C: select", PAL_UI_WHITE);
        break;
    }

    case MODE_COURSE_SELECT: {
        const course_def_t *d = &course_defs[g_gm.course_index];
        int i, n = g_gm.event_mode ? 3 : num_course_defs;
        draw_title_backdrop();
        draw_text_big_shadow(16, 10, g_gm.event_mode ? "Canadian Cup" : "Practice", PAL_UI_YELLOW);
        draw_panel(16, 40, 150, 22 + n * 14);
        for (i = 0; i < n; i++) {
            u8 c = i == g_gm.course_index ? PAL_UI_YELLOW : PAL_UI_WHITE;
            if (g_gm.event_mode && i < g_gm.cup_race) c = PAL_UI_GREEN;
            draw_text_shadow(32, 50 + i * 14, course_defs[i].name, c);
            if (i == g_gm.course_index) draw_text_shadow(22, 50 + i * 14, ">", PAL_UI_YELLOW);
        }
        draw_panel(180, 40, 128, 100);
        draw_text_shadow(188, 48, d->name, PAL_UI_YELLOW);
        draw_text_shadow(188, 66, "Herring:", PAL_UI_WHITE);
        itoa_pad(buf, d->herring_req[g_gm.difficulty], 3);
        draw_text_shadow(188 + 9 * 8, 66, buf, PAL_UI_ORANGE);
        draw_text_shadow(188, 80, "Time:", PAL_UI_WHITE);
        format_time(buf, d->time_req[g_gm.difficulty]);
        draw_text_shadow(188 + 6 * 8, 80, buf, PAL_UI_ORANGE);
        draw_text_shadow(188, 94, "Level:", PAL_UI_WHITE);
        draw_text_shadow(188 + 7 * 8, 94, difficulty_names[g_gm.difficulty], PAL_UI_ORANGE);
        if (g_gm.event_mode) {
            draw_text_shadow(188, 112, "Lives:", PAL_UI_WHITE);
            for (i = 0; i < g_gm.lives; i++) fill_rect(188 + 7 * 8 + i * 10, 113, 6, 6, PAL_UI_WHITE);
        }
        draw_text_shadow(8, SCREEN_H - 24, "Race: Left/Right steer, Up paddle,", PAL_UI_WHITE);
        draw_text_shadow(8, SCREEN_H - 12, "Down brake, A/B/C hold+release: jump", PAL_UI_WHITE);
        break;
    }

    case MODE_LOADING:
        fill_rect(0, 0, SCREEN_W, SCREEN_H, PAL_BLACK);
        draw_text_shadow((SCREEN_W - 8 * 10) / 2, 100, "Loading...", PAL_UI_WHITE);
        break;

    case MODE_INTRO:
        draw_world();
        draw_text_big_shadow((SCREEN_W - 16 * (int)text_width(g_course.def->name) / 8) / 2, 20, g_course.def->name, PAL_UI_YELLOW);
        draw_text_shadow((SCREEN_W - 8 * 20) / 2, SCREEN_H - 20, "Press Start to begin", PAL_UI_WHITE);
        break;

    case MODE_RACING:
        draw_world();
        PROF_BEGIN(6)
        draw_hud();
        PROF_END(6)
        break;

    case MODE_PAUSED:
        draw_world();
        draw_hud();
        draw_panel(100, 90, 120, 44);
        draw_text_shadow(136, 100, "Paused", PAL_UI_YELLOW);
        draw_text_shadow(108, 118, "A: abort race", PAL_UI_WHITE);
        break;

    case MODE_GAME_OVER: {
        const char *msg;
        draw_world();
        draw_panel(60, 50, 200, 120);
        draw_text_big_shadow((SCREEN_W - 16 * 9) / 2, 58, "Race Over", PAL_UI_YELLOW);
        if (!g_game.race_aborted) {
            draw_text_shadow(80, 88, "Time:", PAL_UI_WHITE);
            format_time(buf, g_game.time);
            draw_text_shadow(80 + 9 * 8, 88, buf, PAL_UI_ORANGE);
            draw_text_shadow(80, 102, "Herring:", PAL_UI_WHITE);
            itoa_pad(buf, g_player.herring, 3);
            draw_text_shadow(80 + 9 * 8, 102, buf, PAL_UI_ORANGE);
            draw_text_shadow(80, 116, "Score:", PAL_UI_WHITE);
            itoa_pad(buf, g_player.score, 6);
            draw_text_shadow(80 + 9 * 8, 116, buf, PAL_UI_ORANGE);
        }
        if (g_game.race_aborted) msg = "Race aborted.";
        else if (g_gm.practicing) msg = "";
        else if (g_gm.race_won && g_gm.cup_race >= 2) msg = "You won the cup!";
        else if (g_gm.race_won) msg = "You advanced to the next race!";
        else msg = "You didn't advance.";
        draw_text_shadow((SCREEN_W - text_width(msg)) / 2, 140, msg, PAL_UI_WHITE);
        if (g_gm.mode_frames > 30) draw_text_shadow((SCREEN_W - 8 * 14) / 2, 156, "Press Start", PAL_UI_GREY);
        break;
    }
    }
}
