/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* CHAOS: the logistic drift engine. A drone voice whose pitch wanders on the logistic map
 * x -> r x (1 - x) (May 1976, "Simple mathematical models with very complicated dynamics"):
 * r past the period-doubling cascade (R: 3.6 .. 4.0, Q16), the map stepped at control rate
 * (RATE: its iterations a second, about 0.23 .. 24) — a state word, two multiplies a tick,
 * nothing per sample. The map's value reads as 1/16-semitone offsets around the held note
 * (RANG: up to an octave either side at 127, slewed half a step a block so the wander is
 * continuous), and STEP snaps it to a grid above and below the note (0 free, 127 semitones;
 * the scale's grid is the host's — the contract's track holds only the common parameters and
 * the engine's eight, so the map quantises to its own).
 *   MAP picks the family: LOG the single map; PAIR a second map a little under r, the wander
 * reading the mean of the two: a slower, thicker drift with beats of its own. New maps
 * append (the F_ENUM values are stored).
 *   TONE is the carrier (the table waves), CUT / RES the trapezoidal SVF of dsp.c (low-pass;
 * the host's FLT moves the cutoff). The drive rides R (no DRV knob: deep in the messy regime
 * is also the gritty voice, 1 .. 3 x, ANALOG's idiom); the presets' DIST send does the rest.
 *
 * State: s[0] s[1] the filter, s[4] the map (Q16), s[5] PAIR's, s[6] the slewed offset (1/16
 * semitones); ph[0] the carrier phase, ph[1] the map clock (whole ticks in its top half).
 * keep (0x73) holds the filter and the map over a retrigger: the part's drone never restarts
 * its wander, it goes on from where it stood. note_on seeds the two maps from the note and
 * the unison detune (a note from silence sounds the same every time; UNISON voices split at
 * once). The clock step is (4 + 3 RATE) * 131072 / FS, a division once a block: the same
 * iterations a second at every sample rate, at least 5 (a tick every 4.4 s at 96 kHz) so the
 * drift never stops. x < 2^16 keeps x (1 - x) under 2^30 and x (1 - x) x r under 2^32
 * (unsigned). Q16 traps: x = 1 and 65535 give the absorbing 0 (and 32768 at r = 262143 gives
 * 65535), so the seed stays in 2 .. 65534 and the fold after a step keeps the orbit there —
 * 3, off the ends, runs straight back into the attractor. */
static const char *const N_CHAOS_MAP[] = {"LOG", "PAIR"};
enum { CH_LOG, CH_PAIR };
static const char *const N_CHAOS_TONE[] = {"SIN", "TRI", "SQR", "SAW"};

/* one logistic step (Q16): x -> r x (1 - x) */
static inline uint32_t chaos_step(uint32_t x, uint32_t r)
{
    uint32_t y = ((uint32_t)x * (65536u - x)) >> 16;    /* x (1 - x), Q16: at most 16384 */
    return (y * r) >> 16;                               /* r < 2^18: the product fits */
}

static void chaos_note_on(track_t *t, voice_t *v)
{
    uint32_t seed = (v->note + 1u) * 0x9E3779B9u ^ (uint32_t)v->fine * 0x85EBCA6Bu ^ v->age;
    (void)t;
    v->s[0] = v->s[1] = 0;                               /* filter */
    v->s[4] = (int32_t)(2u + (seed >> 16) % 65533u);   /* the map, off the Q16 absorbing ends */
    v->s[5] = (int32_t)(2u + seed % 65533u);           /* PAIR's */
    v->s[6] = 0;                                         /* the offset starts on the note */
    v->ph[1] = 0;                                        /* the clock: a whole step to the first tick */
}

static void chaos_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t tone = (uint32_t)p[P_E5] & 3u, pair = (uint32_t)p[P_E0] == CH_PAIR, i;
    uint32_t r = 235930u + (uint32_t)(p[P_E1] * 26213 + 63) / 127u;   /* 3.6 .. 4.0 Q16 (one below 4.0) */
    uint32_t step = (uint32_t)((4 + 3 * (int32_t)p[P_E2]) * 131072 / FS);   /* the clock, Q16 a block */
    uint32_t x = (uint32_t)v->s[4], x2 = (uint32_t)v->s[5];
    uint32_t clk = v->ph[1], ph0 = v->ph[0];             /* state in locals: out[] may alias v->s[] */
    int32_t ic1 = v->s[0], ic2 = v->s[1], off = v->s[6];
    if ((clk += step) >= 65536u) {                      /* a tick (step < 2^16: never more than one) */
        uint32_t nx;
        clk -= 65536u;
        nx = chaos_step(x, r);
        x = nx < 2u || nx > 65534u ? 3u : nx;           /* off the ends: 3 runs back into the attractor */
        if (pair) {
            nx = chaos_step(x2, r - 3072u);             /* a little under it: its own windows */
            x2 = nx < 2u || nx > 65534u ? 3u : nx;
        }
    }
    {   /* the block's offset: the map's value around the note, slewed half a step (0 is stable),
         * snapped onto STEP's grid — the heard pitch lands on the grid, and where the rounding
         * goes up, up to half a grid past RANG's end; the snapped value is what the state keeps */
        int32_t span = (int32_t)((p[P_E3] * 192 + 63) / 127);      /* 1/16 st at 127: +-192 */
        off += (mulq15((int32_t)(pair ? (x + x2) >> 1 : x) - 32768, span) - off) >> 1;
        if (p[P_E4]) {
            int32_t g = 1 + (p[P_E4] * 15) / 127;       /* the grid: 1/16 semitone .. a semitone */
            off = ((off + (off >= 0 ? g / 2 : -g / 2)) / g) * g;
        }
    }
    {   /* the carrier's increment, the SVF, the drive on R */
        uint32_t inc = pitch_inc((uint32_t)clamp(m->pitch16 + off, 0, 2047));
        int32_t drive = 32768 + p[P_E1] * 512;          /* 1 .. 3 x, ANALOG's */
        tsvf_t flt;
        tsvf_coef(&flt, (p[P_E6] << 8) + m->cutoff, p[P_E7]);
        if (m->fine)
            inc += (uint32_t)((int32_t)(inc >> 12) * m->fine);
        for (i = 0; i < n; i++) {
            int32_t s;
            switch (tone) {
            case 1:
                s = osc_tri(ph0);
                break;
            case 2:
                s = osc_pulse(ph0, inc, 0x80000000u);
                break;
            case 3:
                s = osc_saw(ph0, inc);
                break;
            default:
                s = osc_sine(ph0);
                break;
            }
            ph0 += inc;
            if (p[P_E1])
                s = softclip(((s >> 2) * (drive >> 2)) >> 11);
            s = soft_knee(tsvf_lp(&flt, s >> 1, &ic1, &ic2), 16000) * 2;
            out[i] += voice_amp(s, m, i) * 2;
        }
    }
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[4] = (int32_t)x;
    v->s[5] = (int32_t)x2;
    v->s[6] = off;
    v->ph[0] = ph0;
    v->ph[1] = clk;
}

/* {MAP, R, RATE, RANG, STEP, TONE, CUT, RES} */
static const preset_t CHAOS_PRESETS[] = {
    {"MURMUR", {CH_LOG, 96, 24, 28, 100, 0, 55, 18}, {80, 100, 115, 90}, 0, 0, FX(0, 30, 25, 75), PAT(5)},
    {"BUTTERFLY", {CH_LOG, 112, 64, 48, 0, 3, 75, 30}, {60, 90, 110, 80}, 0, 0, FX(6, 25, 30, 55), PAT(4)},
    {"COLONY", {CH_PAIR, 127, 92, 64, 60, 2, 90, 55}, {40, 85, 105, 70}, 0, 0, FX(20, 35, 30, 45), PAT(13)},
};

static const engine_t ENG_CHAOS = {
    .name = "CHAOS",
    .page_title = {"MAP", "MOVE"},
    .edit = {
        {"MAP", F_ENUM, 0, 1, 0, N_CHAOS_MAP, 0},
        {"R", F_PCT, 0, 127, 96, 0, 0},
        {"RATE", F_PCT, 0, 127, 40, 0, 0},
        {"RANG", F_PCT, 0, 127, 64, 0, 0},
        {"STEP", F_PCT, 0, 127, 0, 0, 0},
        {"TONE", F_ENUM, 0, 3, 0, N_CHAOS_TONE, 0},
        {"CUT", F_CUTOFF, 0, 127, 80, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .presets = CHAOS_PRESETS,
    .npresets = NELEM(CHAOS_PRESETS),
    .note_on = chaos_note_on,
    .render = chaos_render,
    .knob = {P_E6, P_E3, P_E1, P_REL},
    .keep = 0x73,                /* the filter, the two maps, the slewed offset */
};
