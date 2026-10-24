/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* SHEP: the infinity engine. Shepard-Risset tones: a riser (or faller) that never arrives.
 * Pitch strands sweep through an octave while a windowing envelope fades each in and out;
 * the base tone can climb forever under a held key. Strand phases are constant ratios of
 * one master rate, so up to ~6 strand phases live in the voice's s[0..7] words (keep-bits
 * set), and the amplitude window is read from the existing SINE[1025].
 *   DIR: UP / DOWN / SPIRAL. SPEED: octaves per second (FS-gated). SPREAD: strand density.
 *   TONE: the wave per strand. The remaining three params are the base voice's own
 *   timbre/filter plumbing.
 * Cited: Shepard (1964), Risset (1971).
 *
 * SKELETON (slice 2, in progress): params + presets land; render is a placeholder carrier
 * so the engine compiles and the host renders it. Strand math follows. */
static const char *const N_SHEP_DIR[] = {"UP", "DOWN", "SPIRAL"};
enum { SH_UP, SH_DOWN, SH_SPIRAL };
static const char *const N_SHEP_TONE[] = {"SIN", "TRI", "SQR", "SAW"};

static void shep_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[0] = v->s[1] = 0;     /* filter */
    /* master phase starts at 0: the rise (or fall) continues across retriggers via keep */
    v->s[2] = 0;                /* master phase, Q16: one octave sweep */
    v->s[3] = 0;
    v->ph[0] = 0;               /* carrier phase */
    v->ph[1] = 0;
}

static void shep_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t tone = (uint32_t)p[P_E5] & 3u;
    uint32_t dir = (uint32_t)p[P_E0];
    uint32_t speed = (uint32_t)p[P_E1];
    uint32_t spread = (uint32_t)p[P_E2];
    uint32_t nstrands = 4 + (spread * 2) / 127;   /* 4 .. 6 strands */
    uint32_t i, k;
    int32_t ic1 = v->s[0], ic2 = v->s[1];
    uint32_t master = (uint32_t)v->s[2];           /* Q16, one octave sweep */
    uint32_t ph0 = v->ph[0];
    uint32_t step;                                 /* master advance per block, Q16 */
    int32_t sign = (dir == SH_DOWN) ? -1 : 1;

    /* SPEED: octaves per second, FS-gated. 0..127 -> ~0.03..4 oct/s */
    step = (uint32_t)((int32_t)(speed + 1) * (int32_t)CTL * 65536 / (32 * (int32_t)FS));
    if (step == 0) step = 1;

    tsvf_t flt;
    tsvf_coef(&flt, (p[P_E6] << 8) + m->cutoff, p[P_E7]);

    /* per-block master advance */
    if (sign > 0) master += step * n;   /* approximate: advance over the block */
    else master -= step * n;
    master &= 0xFFFFu;

    for (i = 0; i < n; i++) {
        int32_t acc = 0;
        /* render each strand */
        for (k = 0; k < nstrands; k++) {
            /* strand octave position (Q16): k + master_frac */
            uint32_t frac = master;                       /* Q16 master fraction */
            /* octave offset in 1/16 semitones: 192 * (k + frac/65536) */
            int32_t semis16 = 192 * (int32_t)k + (int32_t)((192u * frac) >> 16);
            int32_t pitch = (int32_t)m->pitch16 + semis16;
            if (pitch < 0) pitch = 0;
            if (pitch > 2047) pitch = 2047;
            uint32_t inc = pitch_inc((uint32_t)pitch);
            /* window position u = (k + frac/65536) / nstrands, half-sine */
            uint32_t u = ((k << 16) + frac) / nstrands;   /* Q16, 0..1+ */
            uint32_t wi = (u * 512u) >> 16;              /* 0..512+ */
            if (wi > 512u) wi = 512u - (wi - 512u);     /* fold back (crude) */
            if (wi > 512u) wi = 512u;
            int32_t wamp = SINE[wi];                      /* 0..32767 */
            /* carrier phase: derive per strand from a base phase incremented by inc.
               Use ph0 shared; advance handled below for primary only. */
            int32_t s;
            uint32_t ph = ph0 + (uint32_t)(k * 0x10000000u);  /* offset phases */
            switch (tone) {
            case 1: s = osc_tri(ph); break;
            case 2: s = osc_pulse(ph, inc, 0x80000000u); break;
            case 3: s = osc_saw(ph, inc); break;
            default: s = osc_sine(ph); break;
            }
            s = (s * (wamp >> 1)) >> 15;                  /* windowed */
            acc += s;
        }
        ph0 += pitch_inc((uint32_t)clamp(m->pitch16, 0, 2047));
        acc = soft_knee(tsvf_lp(&flt, acc >> 1, &ic1, &ic2), 16000) * 2;
        /* SPIRAL: add a slow modulation (placeholder) */
        if (dir == SH_SPIRAL) acc += acc >> 3;
        out[i] += voice_amp(acc, m, i) * 2;
    }

    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = (int32_t)master;
    v->ph[0] = ph0;
}

/* {DIR, SPEED, SPREAD, (base), (base), TONE, CUT, RES} */
static const preset_t SHEP_PRESETS[] = {
    {"ESCALATOR", {SH_UP, 64, 64, 64, 64, 0, 64, 30}, {80, 100, 115, 90}, 0, 0, FX(0, 30, 25, 75), PAT(5)},
    {"NORETURN", {SH_DOWN, 48, 80, 48, 48, 3, 70, 35}, {60, 90, 110, 80}, 0, 0, FX(6, 25, 30, 55), PAT(4)},
};

static const engine_t ENG_SHEP = {
    .name = "SHEP",
    .page_title = {"DIR", "MOVE"},
    .edit = {
        {"DIR", F_ENUM, 0, 2, 0, N_SHEP_DIR, 0},
        {"SPEED", F_PCT, 0, 127, 64, 0, 0},
        {"SPREAD", F_PCT, 0, 127, 64, 0, 0},
        {"BASE1", F_PCT, 0, 127, 64, 0, 0},
        {"BASE2", F_PCT, 0, 127, 64, 0, 0},
        {"TONE", F_ENUM, 0, 3, 0, N_SHEP_TONE, 0},
        {"CUT", F_CUTOFF, 0, 127, 80, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .presets = SHEP_PRESETS,
    .npresets = NELEM(SHEP_PRESETS),
    .note_on = shep_note_on,
    .render = shep_render,
    .knob = {P_E6, P_E3, P_E1, P_REL},
    .keep = 0x0F,                /* filter + strand phases (s[0..3]) */
};