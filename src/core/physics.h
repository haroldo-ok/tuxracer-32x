/*
 * Tux Racer 32X - player physics (port of phys_sim.c) and racing control
 * (port of racing.c).  All quantities in 16.16 fixed point, SI units.
 */
#ifndef PHYSICS_H
#define PHYSICS_H

#include "fixed.h"
#include "course.h"

typedef struct {
    fx turn_fact;          /* [-1,1] */
    fx turn_animation;     /* [-1,1] */
    int is_braking;
    int is_paddling;
    fx paddle_time;
    int jumping;
    int jump_charging;
    int begin_jump;
    fx jump_amt;
    fx jump_start_time;
    fx charge_start_time;
} control_t;

typedef struct {
    vec3 pos;
    vec3 vel;
    vec3 net_force;
    vec3 normal_force;
    quat orientation;
    int orientation_initialized;
    vec3 direction;        /* Tux forward (+y of model) */
    vec3 plane_nml;
    int airborne;
    int collision;
    int herring;
    int score;
    control_t control;
    /* view */
    vec3 view_pos;
    vec3 view_dir;
    vec3 view_up;
    vec3 view_plyr_pos;
    int view_initialized;
    int view_mode;         /* 0 behind, 1 follow, 2 above */
    /* animation inputs computed by update_player_pos */
    fx anim_paddling_factor;
    fx anim_flap_factor;
    fx anim_speed;
    vec3 anim_local_force;
    /* cached */
    fx ode_time_step;
    int last_terrain;      /* bitmask of terrains under tux (bit 3 = airborne) */
} player_t;

typedef struct {
    fx time;               /* race clock (s) */
    int race_over;
    int race_aborted;
    int difficulty;
} game_state_t;

extern player_t g_player;
extern game_state_t g_game;

/* constants */
#define EARTH_GRAV FX(9.81)
#define TUX_MASS FX(20.0)
#define MIN_TUX_SPEED FX(1.4)
#define INIT_TUX_SPEED FX(3.0)
#define JUMP_MAX_START_HEIGHT FX(0.30)

void physics_init(void);              /* init_physical_simulation */
/* racing.c racing_loop input handling + update_player_pos; dt in seconds */
typedef struct {
    int left, right, paddle, brake, charge;
} input_t;
void racing_update(const input_t *in, fx dt);
/* view.c update_view */
void view_update(fx dt);
/* helpers used by other modules */
fx player_speed(void);
void player_set_pos(const vec3 *p);   /* set_tux_pos with clamping / finish detection */

#endif
