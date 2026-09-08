/*
 * Tux Racer 32X - host harness.  Runs the portable game core headlessly
 * with a scripted pad and writes PPM frames, so rendering and game flow
 * can be checked without the emulator.  Usage:
 *   host_main <outdir> [script]
 * script lines: "run N", "press <btn> N", "hold <btn>", "release <btn>", "shot name"
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../core/game.h"
#include "../../core/render.h"
#include "../../core/palette.h"

static u8 fb[SCREEN_W * SCREEN_H];
static render_state_t rs;
static u16 pal[256];
static const char *outdir = ".";

static void write_ppm(const char *name)
{
    char path[512];
    FILE *f;
    int i;
    snprintf(path, sizeof(path), "%s/%s.ppm", outdir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (i = 0; i < SCREEN_W * SCREEN_H; i++) {
        u16 c = pal[fb[i]];
        u8 rgb[3];
        rgb[0] = (u8)((c & 31) << 3);
        rgb[1] = (u8)(((c >> 5) & 31) << 3);
        rgb[2] = (u8)(((c >> 10) & 31) << 3);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("shot %s (mode %d frame %d tris %d)\n", name, g_gm.mode, g_gm.total_frames, rs.num_tris);
}

static u16 btn(const char *s)
{
    if (!strcmp(s, "up")) return BTN_UP;
    if (!strcmp(s, "down")) return BTN_DOWN;
    if (!strcmp(s, "left")) return BTN_LEFT;
    if (!strcmp(s, "right")) return BTN_RIGHT;
    if (!strcmp(s, "a")) return BTN_A;
    if (!strcmp(s, "b")) return BTN_B;
    if (!strcmp(s, "c")) return BTN_C;
    if (!strcmp(s, "start")) return BTN_START;
    return 0;
}

int g_prof_skip;
static u16 held;
static long total_ns;
static int frames_timed;

static void step(int n)
{
    while (n-- > 0) {
        struct timespec t0, t1;
        game_update(held);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        game_render();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        total_ns += (t1.tv_sec - t0.tv_sec) * 1000000000L + (t1.tv_nsec - t0.tv_nsec);
        frames_timed++;
    }
}

int main(int argc, char **argv)
{
    FILE *sf = NULL;
    char line[256];
    if (argc > 1) outdir = argv[1];
    if (argc > 2) sf = fopen(argv[2], "r");
    palette_build(pal);
    render_init(&rs, fb);
    game_init();
    if (!sf) {
        step(200);
        write_ppm("title");
        held = BTN_START; step(1); held = 0; step(30);
        write_ppm("course");
        held = BTN_START; step(1); held = 0; step(60);
        write_ppm("intro");
        step(200);
        write_ppm("race0");
        step(300);
        write_ppm("race1");
        held = BTN_LEFT; step(120); held = 0;
        write_ppm("race2");
        step(600);
        write_ppm("race3");
    } else {
        while (fgets(line, sizeof(line), sf)) {
            char cmd[32], arg[64];
            int n = 0;
            if (sscanf(line, "%31s %63s %d", cmd, arg, &n) < 1) continue;
            if (!strcmp(cmd, "run")) step(atoi(arg));
            else if (!strcmp(cmd, "press")) { held |= btn(arg); step(n > 0 ? n : 1); held &= (u16)~btn(arg); }
            else if (!strcmp(cmd, "hold")) held |= btn(arg);
            else if (!strcmp(cmd, "release")) held &= (u16)~btn(arg);
            else if (!strcmp(cmd, "shot")) write_ppm(arg);
        }
        fclose(sf);
    }
    printf("avg render %.2f ms over %d frames; mode %d time %.2f herring %d pos (%.1f %.1f %.1f)\n",
           frames_timed ? total_ns / 1e6 / frames_timed : 0, frames_timed, g_gm.mode, g_game.time / 65536.0,
           g_player.herring, g_player.pos.x / 65536.0, g_player.pos.y / 65536.0, g_player.pos.z / 65536.0);
    return 0;
}
