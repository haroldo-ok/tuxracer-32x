/* Host simulation test: run the ported physics on the real course data. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/core/physics.h"
#include "../src/core/tuxmodel.h"

static double D(fx v) { return v / 65536.0; }
static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

extern int g_herring_pickup_event;

static void run_course(int ci, int steer_mode, int verbose)
{
    input_t in;
    int frame;
    fx dt = FX(1.0 / 60.0);
    double maxspeed = 0, miny = 1e9;
    int airborne_frames = 0, collisions = 0;
    course_load(ci);
    g_game.time = 0;
    g_game.race_over = 0;
    memset(&g_player, 0, sizeof(g_player));
    g_player.view_mode = 2;
    physics_init();
    view_update(FX(0.0001));
    printf("course %s: %dx%d start (%.1f, %.1f, %.2f) vel (%.2f %.2f %.2f)\n", g_course.def->name,
           g_course.nx, g_course.ny, D(g_player.pos.x), D(g_player.pos.z), D(g_player.pos.y),
           D(g_player.vel.x), D(g_player.vel.y), D(g_player.vel.z));
    for (frame = 0; frame < 60 * 120 && !g_game.race_over; frame++) {
        double spd;
        memset(&in, 0, sizeof(in));
        if (steer_mode == 1) {
            /* weave left/right to test turning */
            in.left = (frame / 90) & 1;
            in.right = !in.left;
        } else if (steer_mode == 2) {
            in.paddle = (frame % 30) == 0;
            in.charge = (frame % 300) < 40;
        } else if (steer_mode == 3) {
            in.brake = 1;
        }
        racing_update(&in, dt);
        view_update(dt);
        spd = D(player_speed());
        if (spd > maxspeed) maxspeed = spd;
        if (D(g_player.pos.y) < miny) miny = D(g_player.pos.y);
        if (g_player.airborne) airborne_frames++;
        if (g_player.collision) { collisions++; g_player.collision = 0; }
        if (verbose && (frame % 300) == 0) {
            printf("  t=%5.1f pos (%6.2f %7.2f %7.2f) spd %5.2f herring %d air %d cam (%.1f %.1f %.1f) dir (%.2f %.2f %.2f)\n",
                   D(g_game.time), D(g_player.pos.x), D(g_player.pos.y), D(g_player.pos.z), spd, g_player.herring,
                   g_player.airborne, D(g_player.view_pos.x), D(g_player.view_pos.y), D(g_player.view_pos.z),
                   D(g_player.view_dir.x), D(g_player.view_dir.y), D(g_player.view_dir.z));
        }
        /* sanity: no NaN-like blowups */
        if (spd > 200) { printf("speed blew up at frame %d\n", frame); break; }
    }
    printf("  finished=%d time=%.2f maxspeed=%.1f m/s herring=%d airborne_frames=%d collisions=%d final z=%.1f\n",
           g_game.race_over, D(g_game.time), maxspeed, g_player.herring, airborne_frames, collisions, D(g_player.pos.z));
    CHECK(maxspeed < 60, "speed plausible (%g)", maxspeed);
    if (steer_mode != 3) CHECK(maxspeed > 8, "tux actually slides (%g)", maxspeed);
    else CHECK(maxspeed < 8, "braking slows tux (%g)", maxspeed);
}

int main(int argc, char **argv)
{
    int verbose = argc > 1;
    int i;
    /* terrain query sanity on bunny hill */
    course_load(0);
    {
        fx y = course_find_y(FX(45.0), FX(-3.5));
        vec3 n;
        course_find_normal(FX(45.0), FX(-3.5), &n);
        printf("bunny hill start y=%.3f normal (%.3f %.3f %.3f)\n", D(y), D(n.x), D(n.y), D(n.z));
        CHECK(n.y > FX(0.5), "normal points up");
        /* slope: y should decrease going downhill */
        CHECK(course_find_y(FX(45.0), FX(-100.0)) < y - FX(30.0), "course slopes down (%.2f)", D(course_find_y(FX(45.0), FX(-100.0))));
        /* compare raw elev formula against the original: ELEV = (pix-127)/255*7 - y/ny*480*tan(25) */
        {
            int gx = 40, gy = 100;
            double pix = g_course.def->elev[gx + g_course.nx * gy];
            double expect = (pix - 127) / 255.0 * 7.0 - gy / 240.0 * 480.0 * 0.4663076582;
            double got = D(course_elev(gx, gy));
            CHECK(got > expect - 0.05 && got < expect + 0.05, "elev formula %g vs %g", got, expect);
        }
    }
    /* Tux model evaluation sanity */
    {
        tux_pose_t pose;
        mat34 root, frames[TUX_NUM_FRAMES];
        vec3 lf = {0, 0, 0};
        int k;
        double miny = 1e9, maxy = -1e9;
        m34_identity(&root);
        tux_pose_racing(&pose, 0, 0, 0, FX(10.0), &lf, 0);
        tux_eval_frames(&pose, &root, frames);
        for (k = 0; k < TUX_NUM_FRAMES; k++) {
            int j;
            for (j = 0; j < tux_mesh_lod0[k].num_verts; j++) {
                vec3 c;
                m34_apply(&c, &frames[k], &tux_mesh_lod0[k].verts[j]);
                if (D(c.y) < miny) miny = D(c.y);
                if (D(c.y) > maxy) maxy = D(c.y);
            }
        }
        printf("tux y range [%.2f, %.2f]\n", miny, maxy);
        CHECK(maxy - miny > 0.4 && maxy - miny < 1.5, "tux size plausible (0.35 scale)");
    }
    for (i = 0; i < num_course_defs; i++) run_course(i, 0, verbose);
    run_course(0, 1, verbose);
    run_course(0, 2, verbose);
    run_course(0, 3, verbose);
    if (fails) { printf("%d FAILURES\n", fails); return 1; }
    printf("test_physics: all OK\n");
    return 0;
}
