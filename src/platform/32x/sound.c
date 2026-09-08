/*
 * Tux Racer 32X - PWM sample player, runs entirely on the slave SH2.
 *
 * Plays the original Tux Racer sound effects (converted to 8-bit mono
 * 11025 Hz by tools/convert_sounds.py) through the 32X PWM at ~22 kHz:
 *   - one looping "sliding" layer (tux_on_snow/ice/rock) whose volume
 *     follows Tux's speed
 *   - up to 3 one-shot voices (fish pickup, tree hit, menu blip, finish)
 *
 * Everything lives in stack locals / const ROM tables: the slave must
 * not touch .data/.bss, since the master clears those during its own
 * startup while the slave is already running.
 */
#include "mars.h"
#include "sound.h"

/* 23.01 MHz NTSC SH2 clock / 1044 = ~22 kHz output */
#define PWM_CYCLE_REG   1045
#define PWM_CENTER      522
#define SAMPLE_RATE     22050
#define BATCH           64
#define MAGIC_VIDEO     0x474F      /* master's "video up" COMM0 magic ('GO') */
#define NUM_VOICES      3

typedef struct {
    const s8 *data;
    int len;
    int pos;        /* 16.16 sample position */
    int step;       /* 16.16 */
    int vol;        /* 0..64 */
} voice_t;

typedef struct {
    voice_t v[NUM_VOICES];
    voice_t slide;
    int slide_surf;
    int slide_vol;
    u8 last_seq;
    int menu_t;
    u32 menu_ph;
    int finish_t;
    u32 finish_ph;
} synth_t;

static void start_voice(voice_t *v, int snd, int vol)
{
    v->data = sound_defs[snd].data;
    v->len = sound_defs[snd].len;
    v->pos = 0;
    v->step = (11025 << 16) / SAMPLE_RATE;
    v->vol = vol;
}

static void trigger_event(synth_t *s, int ev)
{
    int i, best = 0;
    /* pick a free voice, else the one furthest along */
    for (i = 0; i < NUM_VOICES; i++) {
        if (s->v[i].data == 0) { best = i; break; }
        if ((s->v[i].pos >> 16) > (s->v[best].pos >> 16)) best = i;
    }
    switch (ev) {
    case SND_EV_FISH:   start_voice(&s->v[best], SND_FISH, 64); break;
    case SND_EV_TREE:   start_voice(&s->v[best], SND_HIT_TREE, 64); break;
    case SND_EV_MENU:   s->menu_t = SAMPLE_RATE / 20; s->menu_ph = 0; break;
    case SND_EV_FINISH: s->finish_t = SAMPLE_RATE / 2; s->finish_ph = 0; break;
    default: break;
    }
}

static inline int voice_sample(voice_t *v, int loop)
{
    int idx, smp;
    if (v->data == 0) return 0;
    idx = v->pos >> 16;
    if (idx >= v->len) {
        if (!loop) { v->data = 0; return 0; }
        v->pos -= v->len << 16;
        idx = v->pos >> 16;
        if (idx < 0) idx = 0;
    }
    smp = v->data[idx];             /* -128..127 */
    v->pos += v->step;
    return (smp * v->vol) << 2;     /* -> ~16-bit range at vol 64 */
}

static int render_sample(synth_t *s)
{
    int mix = 0, i;
    for (i = 0; i < NUM_VOICES; i++)
        mix += voice_sample(&s->v[i], 0);
    if (s->slide.data)
        mix += voice_sample(&s->slide, 1);
    if (s->menu_t > 0) {
        s->menu_ph += (1200 * 65536) / SAMPLE_RATE;
        mix += (s->menu_ph & 0x8000) ? 0x1800 : -0x1800;
        s->menu_t--;
    }
    if (s->finish_t > 0) {
        /* rising two-note chime */
        int half = SAMPLE_RATE / 4;
        int inc = (s->finish_t > half) ? (988 * 65536) / SAMPLE_RATE : (1318 * 65536) / SAMPLE_RATE;
        int amp = 0x2000;
        int t = s->finish_t > half ? s->finish_t - half : s->finish_t;
        if (t < 1024) amp = (amp * t) >> 10;
        s->finish_ph += inc;
        mix += (s->finish_ph & 0x8000) ? amp : -amp;
        s->finish_t--;
    }
    if (mix > 32000) mix = 32000;
    if (mix < -32000) mix = -32000;
    return mix;
}

void slave_main(void)
{
    synth_t s;
    unsigned i;
    u8 *p = (u8 *)&s;
    for (i = 0; i < sizeof(s); i++) p[i] = 0;
    s.last_seq = 0xFF;
    s.slide_surf = 0;

    /* wait until the master owns the system and video is up, so PWM
       init can't race the reset/handshake sequence */
    while (MARS_SYS_COMM0 != MAGIC_VIDEO)
        ;

    MARS_PWM_CYCLE = PWM_CYCLE_REG;
    MARS_PWM_CTRL  = 0x0005;        /* L normal, R normal */

    for (;;) {
        u16 ctl = MARS_SYS_COMM6;
        if (ctl & 0x8000) {
            int seq = (ctl >> 3) & 7;
            int ev = ctl & 7;
            int surf = (ctl >> 6) & 3;
            int vol = (ctl >> 8) & 63;
            if (seq != s.last_seq) {
                s.last_seq = (u8)seq;
                if (ev != SND_EV_NONE) trigger_event(&s, ev);
            }
            if (surf != s.slide_surf) {
                s.slide_surf = surf;
                if (surf == 0) s.slide.data = 0;
                else start_voice(&s.slide, surf == 1 ? SND_ON_SNOW : surf == 2 ? SND_ON_ICE : SND_ON_ROCK, vol);
            }
            s.slide.vol = vol;
        }
        for (i = 0; i < BATCH; i++) {
            int smp = render_sample(&s);
            int out = PWM_CENTER + ((smp * PWM_CENTER) >> 15);
            if (out < 1) out = 1;
            if (out > PWM_CYCLE_REG - 2) out = PWM_CYCLE_REG - 2;
            while ((MARS_PWM_LCH & MARS_PWM_FULL) || (MARS_PWM_RCH & MARS_PWM_FULL))
                ;
            MARS_PWM_LCH = (u16)out;
            MARS_PWM_RCH = (u16)out;
        }
    }
}
