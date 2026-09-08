/*
 * Tux Racer 32X - sound: sample playback on the slave SH2 through the PWM.
 *
 * The master publishes a sound-state word in COMM6 every frame:
 *   bits 0..2  : one-shot event (SND_EV_*), bits 3..5 : event sequence
 *   bits 6..7  : sliding surface (0 none, 1 snow, 2 ice, 3 rock)
 *   bits 8..13 : sliding volume (0..63, from speed)
 *   bit 15     : valid
 * COMM4 keeps the boot handshake (S_OK) and is never reused.
 */
#ifndef SOUND_H
#define SOUND_H

#include "../../core/fixed.h"

typedef struct {
    const s8 *data;
    int len;
} sound_def_t;

extern const sound_def_t sound_defs[];
extern const int num_sound_defs;

/* indices into sound_defs (order of tools/convert_sounds.py) */
#define SND_FISH      0
#define SND_HIT_TREE  1
#define SND_ON_SNOW   2
#define SND_ON_ICE    3
#define SND_ON_ROCK   4

#define SND_EV_NONE   0
#define SND_EV_FISH   1
#define SND_EV_TREE   2
#define SND_EV_MENU   3
#define SND_EV_FINISH 4

#define SND_WORD(ev, seq, surf, vol) \
    ((u16)(0x8000 | ((ev) & 7) | (((seq) & 7) << 3) | (((surf) & 3) << 6) | (((vol) & 63) << 8)))

/* slave SH2 entry (never returns) */
void slave_main(void);

#endif
