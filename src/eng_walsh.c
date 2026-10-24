/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* WALSH: orthogonal function synthesis. Tones built from Walsh/Hadamard
 * functions (sign-flipping square basis) instead of sines: glassy,
 * transistor-organ wrongness.
 *   ORDER: the basis count, 2^n. SCAN: sweep through order space mid-note.
 *   GLIDE / TONE / the rest: voice plumbing from the neighbours.
 * A small const table carries the Rademacher/Gray-code sign patterns;
 * zero rate-specific constants.
 *
 * SKELETON (slice 3, in progress): params + presets + a Walsh render that
 * sums the first 2^ORDER Hadamard rows; SCAN offsets the basis set.
 * Full scan/scan-modulation and timbre plumbing follow. */
static const char *const N_WALSH_TONE[] = {"SIN", "TRI", "SQR", "SAW"};

/* Gray code of an index */
static uint32_t walsh_gray(uint32_t x) { return x ^ (x >> 1); }

/* value (+1/-1) of Walsh function `k` at phase position `pos` (0..65535) */
static int32_t walsh_sign(uint32_t k, uint32_t pos)
{
    uint32_t g = walsh_gray(pos >> 8);   /* 8-bit position -> 256 columns */
    /* parity of (k & g) */
    uint32_t v = k & g;
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (v & 1u) ? -1 : 1;
}

static void walsh_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[0] = v->s[1] = 0;     /* filter */
    v->ph[0] = 0;
}

static void walsh_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t order = (uint32_t)p[P_E0];            /* 0..4 -> 2..32 basis */
    uint32_t scan = (uint32_t)p[P_E1];
    uint32_t nbasis = 1u << (order + 1);
    if (nbasis > 32u) nbasis = 32u;
    uint32_t offset = (scan * 31u) >> 7;           /* 0..31 basis offset */
    uint32_t tone = (uint32_t)p[P_E5] & 3u;
    int32_t ic1 = v->s[0], ic2 = v->s[1];
    uint32_t ph = v->ph[0];

    tsvf_t flt;
    tsvf_coef(&flt, (p[P_E6] << 8) + m->cutoff, p[P_E7]);

    uint32_t i;
    for (i = 0; i < n; i++) {
        /* advance carrier phase */
        uint32_t inc = pitch_inc((uint32_t)clamp(m->pitch16, 0, 2047));
        ph += inc;
        uint32_t col = ph >> 16;                    /* 0..65535 -> folded to 8-bit in walsh_sign */

        int32_t acc = 0;
        uint32_t k;
        for (k = 0; k < nbasis; k++) {
            uint32_t idx = (k + offset) & 31u;
            int32_t sgn = walsh_sign(idx, col);
            /* optional tone colouring: square-ish base */
            int32_t val = sgn * (32767 / (int32_t)nbasis);
            acc += val;
        }
        /* blend with a sine tone for TONE knob flavour */
        if (tone == 0) {
            acc = (acc + osc_sine(ph)) >> 1;
        }
        acc = soft_knee(tsvf_lp(&flt, acc, &ic1, &ic2), 16000);
        out[i] += voice_amp(acc, m, i);
    }

    v->s[0] = ic1;
    v->s[1] = ic2;
    v->ph[0] = ph;
}

/* {ORDER, SCAN, (base), (base), (base), TONE, CUT, RES} */
static const preset_t WALSH_PRESETS[] = {
    {"GLASS", {2, 0, 64, 64, 64, 0, 80, 30}, {80, 100, 115, 90}, 0, 0, FX(0, 30, 25, 75), PAT(5)},
    {"ORGAN", {3, 64, 64, 64, 64, 2, 70, 35}, {60, 90, 110, 80}, 0, 0, FX(6, 25, 30, 55), PAT(4)},
};

static const engine_t ENG_WALSH = {
    .name = "WALSH",
    .page_title = {"ORDER", "SCAN"},
    .edit = {
        {"ORDER", F_INT, 0, 4, 2, 0, 0},
        {"SCAN", F_PCT, 0, 127, 0, 0, 0},
        {"BASE1", F_PCT, 0, 127, 64, 0, 0},
        {"BASE2", F_PCT, 0, 127, 64, 0, 0},
        {"BASE3", F_PCT, 0, 127, 64, 0, 0},
        {"TONE", F_ENUM, 0, 3, 0, N_WALSH_TONE, 0},
        {"CUT", F_CUTOFF, 0, 127, 80, 0, 0},
        {"RES", F_PCT, 0, 127, 30, 0, 0},
    },
    .presets = WALSH_PRESETS,
    .npresets = NELEM(WALSH_PRESETS),
    .note_on = walsh_note_on,
    .render = walsh_render,
    .knob = {P_E6, P_E3, P_E1, P_REL},
    .keep = 0x03,                /* filter (s[0..1]) */
};
