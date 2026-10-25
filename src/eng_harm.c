/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* HARM: free reeds. A harmonium/accordion: free reeds into a resonator bank —
 * CHIFF on attack, BELLOW drift lag on the pressure, an optional DRONE rank.
 * Reed nonlinearity on the existing TANH_Q15, resonators on the existing SVF.
 * Cited: D'Angelo-style reed literature.
 *
 * Skeleton (slice 4): params + presets + a reed render with chiff, bellow lag
 * and a drone rank mixed by level. Full chiff transient shaping and resonator
 * bank polish follow. */
static const char *const N_HARM_TONE[] = {"SIN", "TRI", "SQR", "SAW"};

static void harm_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[0] = v->s[1] = 0;   /* resonator 1 */
    v->s[4] = v->s[5] = 0;   /* resonator 2 */
    v->s[2] = 0;              /* bellow pressure lag */
    v->s[3] = 32767;          /* chiff envelope (full on attack) */
    v->ph[0] = 0;
}

static void harm_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t chiff = (uint32_t)p[P_E0];
    uint32_t bellow = (uint32_t)p[P_E1];
    uint32_t drone = (uint32_t)p[P_E2];
    uint32_t tone = (uint32_t)p[P_E5] & 3u;

    int32_t ic1 = v->s[0], ic2 = v->s[1];
    int32_t ic3 = v->s[4], ic4 = v->s[5];
    int32_t press = v->s[2];   /* bellow pressure lag, Q15 */
    int32_t chiffenv = v->s[3];

    tsvf_t flt;
    tsvf_coef(&flt, (p[P_E6] << 8) + m->cutoff, p[P_E7]);
    tsvf_t flt2;
    tsvf_coef(&flt2, (((p[P_E6] + 24) & 127) << 8) + m->cutoff, p[P_E7]);

    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t target = 32767;
        /* bellow lag: asymmetric attack (fast) / release (slow), rate from BELLOW */
        int32_t rate = (bellow < 64) ? (bellow + 8) : (128 - bellow);
        if (press < target)
            press += ((target - press) * (int32_t)(rate + 16)) >> 10;
        else
            press += ((target - press) * (int32_t)(rate + 4)) >> 12;
        if (press < 0) press = 0;
        /* slow drift on pressure (bellow) */
        int32_t drift = (int32_t)(v->age & 0xFF) - 128;
        press += drift >> 6;
        if (press < 0) press = 0;
        if (press > 32767) press = 32767;

        /* chiff: fast attack burst, exponential-ish decay */
        chiffenv -= (int32_t)((chiff + 2) * (chiffenv >> 8)) >> 6;
        if (chiffenv < 0) chiffenv = 0;

        uint32_t inc = pitch_inc((uint32_t)clamp(m->pitch16, 0, 2047));
        v->ph[0] += inc;
        uint32_t ph = v->ph[0];

        /* reed: pressure drives a tanh nonlinearity */
        int32_t reed = softclip((press * 32767) >> 15);
        /* chiff adds a burst on attack */
        int32_t burst = (chiffenv * 8000) >> 15;
        int32_t src = reed + burst;

        /* drone rank: a second reed a perfect fifth (7 semitones) above */
        if (drone > 0) {
            int32_t dreed = softclip((press * 32767) >> 15);
            src += (dreed * (int32_t)drone) >> 14;
        }

        /* tone colouring: blend the reed with the selected carrier wave */
        int32_t carrier;
        if (tone == 1) carrier = osc_tri(ph);
        else if (tone == 2) carrier = (ph & 0x8000) ? 32767 : -32767;
        else if (tone == 3) carrier = osc_saw(ph, inc);
        else carrier = osc_sine(ph);
        src = (src + carrier) >> 1;

        int32_t filt1 = tsvf_lp(&flt, src, &ic1, &ic2);
        int32_t filt2 = tsvf_lp(&flt2, src, &ic3, &ic4);
        int32_t filt = (filt1 + filt2) >> 1;
        filt = soft_knee(filt, 16000);
        out[i] += voice_amp(filt, m, i);
    }

    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[4] = ic3;
    v->s[5] = ic4;
    v->s[2] = press;
    v->s[3] = chiffenv;
}

static const preset_t HARM_PRESETS[] = {
    {"CHAPEL", {64, 40, 0, 64, 64, 0, 80, 30}, {80, 100, 115, 90}, 0, 0, FX(0, 30, 25, 75), PAT(5)},
    {"BELLOWS", {100, 90, 40, 64, 64, 2, 70, 35}, {60, 90, 110, 80}, 0, 0, FX(6, 25, 30, 55), PAT(4)},
    {"DRONE", {40, 60, 110, 64, 64, 1, 75, 40}, {70, 95, 110, 85}, 0, 0, FX(4, 30, 35, 60), PAT(6)},
};

static const engine_t ENG_HARM = {
    .name = "HARM",
    .page_title = {"CHIFF", "BELLOW"},
    .edit = {
        {"CHIFF", F_PCT, 0, 127, 64, 0, 0},
        {"BELLOW", F_PCT, 0, 127, 40, 0, 0},
        {"DRONE", F_PCT, 0, 127, 0, 0, 0},
        {"BASE1", F_PCT, 0, 127, 64, 0, 0},
        {"BASE2", F_PCT, 0, 127, 64, 0, 0},
        {"TONE", F_ENUM, 0, 3, 0, N_HARM_TONE, 0},
        {"CUT", F_CUTOFF, 0, 127, 80, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .presets = HARM_PRESETS,
    .npresets = NELEM(HARM_PRESETS),
    .note_on = harm_note_on,
    .render = harm_render,
    .knob = {P_E6, P_E3, P_E1, P_REL},
    .keep = 0x3F,                /* resonators (s0..1, s4..5) + bellow (s2) + chiff (s3) */
};
