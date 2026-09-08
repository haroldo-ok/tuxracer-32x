/*
 * Tux Racer 32X - master SH2 entry point and main loop.
 *
 * Boot / COMM slot map (keep this in sync with src-md/m68k.s and sound.c):
 *   COMM0  master magic ('TX' = C reached, 'GO' = video up) - test telemetry
 *   COMM2  frame heartbeat (game ticks)
 *   COMM4  boot handshake (S_OK from the slave) - never reused
 *   COMM6  sound state word (master -> slave, see sound.h)
 *   COMM8  pad state, published by the 68000 every vblank
 *   COMM10 68000 vblank counter
 *   COMM12 game mode (low byte) | course (high byte)
 *   COMM14 Tux speed (m/s, 8.8)
 */
#include "mars.h"
#include "sound.h"
#include "../../core/game.h"
#include "../../core/render.h"
#include "../../core/palette.h"

#define TEST_MAGIC_BOOT   0x5458     /* 'TX' - SH2 C code reached */
#define TEST_MAGIC_VIDEO  0x474F     /* 'GO' - video initialised */

/* the master renders straight into the 32X framebuffer (uncached, 16-bit
   writes only - the rasterizer respects that), so no SDRAM back buffer */
static render_state_t g_render_state;
int g_prof_skip;
#ifdef DEBUG_OVERLAY
/* SH2 free-running timer: 16-bit, clocked at sysclk/8 (TCR=0) -> ~2.9 MHz */
static inline unsigned frt_read(void)
{
    volatile uint8_t *frc = (volatile uint8_t *)0xFFFFFE12;
    unsigned hi = frc[0], lo = frc[1];
    return (hi << 8) | lo;
}
unsigned g_prof_stage[8];
#endif
static u16 g_palette[256];

static void init_linetable(void)
{
    int i;
    volatile uint16_t *lt = MARS_FRAMEBUFFER;
    for (i = 0; i < 224; i++)
        lt[i] = (uint16_t)(i * (SCREEN_W / 2) + 0x100);
    for (; i < 256; i++)
        lt[i] = (uint16_t)(223 * (SCREEN_W / 2) + 0x100);
}

static void flip(void)
{
    uint16_t cur = MARS_VDP_FBCTL & MARS_VDP_FS;
    uint16_t want = cur ^ MARS_VDP_FS;
    uint32_t guard = 0;
    MARS_VDP_FBCTL = want;
    while ((MARS_VDP_FBCTL & MARS_VDP_FS) != want && ++guard < 1000000)
        ;
}

static void init_video(void)
{
    int i;
    uint32_t guard;

    /* wait until the 68000 hands the VDP over to us (FM=1) */
    for (guard = 0; guard < 4000000; guard++)
        if (MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP) break;

    MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;

    /* build the line table and clear both framebuffers */
    for (i = 0; i < 2; i++) {
        volatile uint32_t *p = (volatile uint32_t *)MARS_FB_PIXELS;
        int n;
        init_linetable();
        for (n = 0; n < SCREEN_W * 224 / 4; n++) p[n] = 0;
        flip();
    }
}

static void set_palette(void)
{
    int i;
    uint32_t guard;
    palette_build(g_palette);
    /* CRAM is only safely writable during vblank */
    for (guard = 0; guard < 4000000; guard++)
        if (MARS_VDP_FBCTL & MARS_VDP_VBLK) break;
    for (i = 0; i < 256; i++)
        MARS_CRAM[i] = g_palette[i];
}

int main(void)
{
    uint16_t frame = 0;
    uint16_t lastvb;
    uint32_t timeout;
    u8 sfx_seq = 0;
#ifdef DEBUG_OVERLAY
    uint16_t prof_t0 = 0, prof_t1 = 0, prof_t2 = 0, prof_t3 = 0;
#endif

    /* The boot ROM protocol: the master writes M_OK to COMM0 and the
       slave polls COMM0 for it before writing S_OK to COMM4. Don't
       clobber COMM0 until the slave has checked in (bounded wait so a
       missing slave can never deadlock the game). */
    for (timeout = 0; timeout < 2000000; timeout++) {
        if (MARS_SYS_COMM4 == 0x535F)   /* 'S_' of "S_OK" */
            break;
    }

    MARS_SYS_COMM0 = TEST_MAGIC_BOOT;

    init_video();
    set_palette();
#ifdef DEBUG_OVERLAY
    *(volatile uint8_t *)0xFFFFFE16 = 0x00;     /* FRT: phi/8 */
    *(volatile uint8_t *)0xFFFFFE10 = 0x00;     /* TIER: no interrupts */
#endif

    render_init(&g_render_state, MARS_FB_PIXELS);
    game_init();
#ifdef BENCH
    /* micro-benchmark build (make bench): never a shippable ROM */
    {
        extern void run_bench(void);
        extern unsigned g_bench[16];
        int i;
        MARS_SYS_COMM0 = TEST_MAGIC_VIDEO;
        run_bench();
        for (;;) {
            fill_rect(0, 0, SCREEN_W, SCREEN_H, PAL_BLACK);
            for (i = 0; i < 16; i++) draw_number(8 + (i / 8) * 80, 20 + (i % 8) * 12, g_bench[i], 6, PAL_UI_WHITE);
            for (i = 0; i < 16; i++) {
                int b;
                for (b = 0; b < 16; b++)
                    fill_rect(i * 20 + b, 220, 1, 4, (g_bench[i] >> (15 - b)) & 1 ? PAL_UI_WHITE : PAL_UI_DKGREY);
            }
            flip();
        }
    }
#endif

    /* video up: also releases the slave's PWM player (it waits for 'GO') */
    MARS_SYS_COMM0 = TEST_MAGIC_VIDEO;

    lastvb = MARS_SYS_COMM10;
    for (;;) {
        uint16_t pad = MARS_SYS_COMM8 & 0x00FF;   /* 3-button subset */
        uint16_t nowvb = MARS_SYS_COMM10;
        uint16_t elapsed = (uint16_t)(nowvb - lastvb);
        int sfx = 0;
#ifdef DEBUG_OVERLAY
        int elapsed_dbg = elapsed;
        int ticks_this_frame;
#endif
        lastvb = nowvb;

        /* run gameplay at a fixed 60 Hz clocked by the 68000 vblank
           counter, independent of how long a frame takes to render */
        if (elapsed < 1) elapsed = 1;
        if (elapsed > 6) elapsed = 6;
#ifdef PROF_SKIP
        if (PROF_SKIP & 16) elapsed = 1;     /* profiling: one logic tick per frame */
#endif
#ifdef DEBUG_OVERLAY
        prof_t0 = MARS_SYS_COMM10;
        ticks_this_frame = elapsed;
#endif
        while (elapsed--) {
            game_update(pad);
            sfx |= g_gm.sfx;
            frame++;
            MARS_SYS_COMM2 = frame;
        }
#ifdef DEBUG_OVERLAY
        prof_t1 = MARS_SYS_COMM10;
#endif

        /* render the current frame into the back buffer and show it */
#ifdef PROF_SKIP
        g_prof_skip = PROF_SKIP;
#endif
        game_render();
#ifdef DEBUG_OVERLAY
        {
            /* cumulative counters (u16, wrap): tools/profile.py differences
               two screenshots to get per-frame / per-tick averages */
            static uint16_t upd_sum, rnd_sum, flp_sum, frames_sum, ticks_sum;
            prof_t2 = MARS_SYS_COMM10;
            upd_sum += (uint16_t)(prof_t1 - prof_t0);
            rnd_sum += (uint16_t)(prof_t2 - prof_t1);
            flp_sum += (uint16_t)(prof_t0 - prof_t3);
            frames_sum++;
            ticks_sum += (uint16_t)ticks_this_frame;
            fill_rect(0, 96, 60, 64, PAL_BLACK);
            draw_number(4, 100, frame, 6, PAL_UI_WHITE);
            draw_number(4, 110, nowvb, 6, PAL_UI_WHITE);
            draw_number(4, 120, elapsed_dbg, 3, PAL_UI_WHITE);
            draw_number(4, 130, g_render_state.num_tris, 5, PAL_UI_WHITE);
            {
                int i;
                unsigned vals[16];
                vals[0] = frame; vals[1] = nowvb; vals[2] = frames_sum; vals[3] = g_render_state.num_tris;
                vals[4] = upd_sum; vals[5] = rnd_sum; vals[6] = flp_sum;
                for (i = 0; i < 7; i++) vals[7 + i] = g_prof_stage[i];
                vals[14] = ticks_sum;
                vals[15] = 0xA55A;
                for (i = 0; i < 16; i++) {
                    int b;
                    for (b = 0; b < 16; b++) {
                        u8 c = (vals[i] >> (15 - b)) & 1 ? PAL_UI_WHITE : PAL_UI_DKGREY;
                        fill_rect(i * 20 + b, 220, 1, 4, c);
                    }
                }
            }
        }
#endif
        flip();
#ifdef DEBUG_OVERLAY
        prof_t3 = MARS_SYS_COMM10;
#endif

        /* publish sound state for the slave SH2 */
        {
            int ev = SND_EV_NONE, surf = 0, vol = 0;
            if (sfx & SFX_TREE) ev = SND_EV_TREE;
            else if (sfx & SFX_HERRING) ev = SND_EV_FISH;
            else if (sfx & SFX_FINISH) ev = SND_EV_FINISH;
            else if (sfx & SFX_MENU) ev = SND_EV_MENU;
            if (ev != SND_EV_NONE) sfx_seq = (u8)((sfx_seq + 1) & 7);
            if (g_gm.mode == MODE_RACING) {
                fx spd = player_speed();
                vol = spd >> 15;                /* 2 per m/s */
                if (vol > 63) vol = 63;
                if (sfx & SFX_SNOW) surf = 1;
                else if (sfx & SFX_ICE) surf = 2;
                else if (sfx & SFX_ROCK) surf = 3;
                if (vol < 4) surf = 0;
            }
            MARS_SYS_COMM6 = SND_WORD(ev, sfx_seq, surf, vol);
        }

        /* telemetry for the automated tests */
        MARS_SYS_COMM12 = (uint16_t)((g_gm.course_index << 8) | (g_gm.mode & 0xFF));
        MARS_SYS_COMM14 = (uint16_t)(player_speed() >> 8);
    }
    return 0;
}
