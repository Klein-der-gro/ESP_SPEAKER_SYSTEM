#include "dsp.h"

#include <math.h>
#include <string.h>

#define DSP_PI 3.14159265358979323846f
#define EQ_BASS_HZ 120.0f
#define EQ_TREBLE_HZ 7000.0f
#define EQ_VOCAL_HZ 2600.0f
#define EQ_VOCAL_Q 0.9f
#define EQ_GAIN_SMOOTHING_SECONDS 0.080f
#define EQ_RECALC_THRESHOLD_DB 0.05f
#define VOCAL_LOW_HZ 120.0f
#define VOCAL_HIGH_HZ 5000.0f
#define VOCAL_FILTER_Q 0.70710678f
#define VOCAL_BACKGROUND_LEVEL 0.25f
/* Center cancellation cannot remove off-center vocals or strong reverb reliably. */
#define VOCAL_REMOVE_AMOUNT 0.95f
#define VOCAL_REMOVE_LEVEL_COMP 1.10f
#define LIMITER_THRESHOLD 26028.0f
#define LIMITER_ATTACK_SECONDS 0.001f
#define LIMITER_RELEASE_SECONDS 0.150f
#define GAIN_RAMP_SECONDS 0.015f
#define LOOKAHEAD_MAX_FRAMES 48U
#define LOOKAHEAD_SECONDS 0.001f

enum {
    VOCAL_MODE_OFF = 0,
    VOCAL_MODE_ISOLATE = 1,
    VOCAL_MODE_REMOVE = 2
};

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
} biquad_coefficients_t;

typedef struct {
    float z1;
    float z2;
} biquad_state_t;

typedef struct {
    uint32_t sample_rate;
    float target_bass_db;
    float target_treble_db;
    float target_vocal_db;
    float bass_db;
    float treble_db;
    float vocal_db;
    float coefficient_bass_db;
    float coefficient_treble_db;
    float coefficient_vocal_db;
    float target_volume_gain;
    float target_left_balance;
    float target_right_balance;
    float target_mute_gain;
    float volume_gain;
    float left_balance;
    float right_balance;
    float mute_gain;
    int vocal_mode;
    float limiter_gain;
    float limiter_attack;
    float limiter_release;
    float gain_ramp;
    size_t lookahead_frames;
    size_t lookahead_index;
    float lookahead[LOOKAHEAD_MAX_FRAMES][2];
    uint32_t random_state;
    biquad_coefficients_t bass_eq;
    biquad_coefficients_t treble_eq;
    biquad_coefficients_t vocal_eq;
    biquad_coefficients_t vocal_hp;
    biquad_coefficients_t vocal_lp;
    biquad_state_t bass_state[2];
    biquad_state_t treble_state[2];
    biquad_state_t vocal_eq_state[2];
    biquad_state_t vocal_hp_state[2];
    biquad_state_t vocal_lp_state[2];
} dsp_state_t;

static dsp_state_t s_dsp;

static biquad_coefficients_t identity_coefficients(void)
{
    return (biquad_coefficients_t) {
        .b0 = 1.0f,
        .b1 = 0.0f,
        .b2 = 0.0f,
        .a1 = 0.0f,
        .a2 = 0.0f,
    };
}

static biquad_coefficients_t make_shelf(uint32_t sample_rate, float frequency,
                                        float gain_db, bool low_shelf)
{
    if (sample_rate == 0U || fabsf(gain_db) < 0.0001f) {
        return identity_coefficients();
    }
    float nyquist_limit = (float)sample_rate * 0.45f;
    if (frequency > nyquist_limit) frequency = nyquist_limit;

    float amplitude = powf(10.0f, gain_db / 40.0f);
    float omega = 2.0f * DSP_PI * frequency / (float)sample_rate;
    float cosine = cosf(omega);
    float alpha = sinf(omega) * 0.5f * 1.41421356f;
    float beta = 2.0f * sqrtf(amplitude) * alpha;
    float b0;
    float b1;
    float b2;
    float a0;
    float a1;
    float a2;

    if (low_shelf) {
        b0 = amplitude * ((amplitude + 1.0f) -
             (amplitude - 1.0f) * cosine + beta);
        b1 = 2.0f * amplitude * ((amplitude - 1.0f) -
             (amplitude + 1.0f) * cosine);
        b2 = amplitude * ((amplitude + 1.0f) -
             (amplitude - 1.0f) * cosine - beta);
        a0 = (amplitude + 1.0f) +
             (amplitude - 1.0f) * cosine + beta;
        a1 = -2.0f * ((amplitude - 1.0f) +
             (amplitude + 1.0f) * cosine);
        a2 = (amplitude + 1.0f) +
             (amplitude - 1.0f) * cosine - beta;
    } else {
        b0 = amplitude * ((amplitude + 1.0f) +
             (amplitude - 1.0f) * cosine + beta);
        b1 = -2.0f * amplitude * ((amplitude - 1.0f) +
             (amplitude + 1.0f) * cosine);
        b2 = amplitude * ((amplitude + 1.0f) +
             (amplitude - 1.0f) * cosine - beta);
        a0 = (amplitude + 1.0f) -
             (amplitude - 1.0f) * cosine + beta;
        a1 = 2.0f * ((amplitude - 1.0f) -
             (amplitude + 1.0f) * cosine);
        a2 = (amplitude + 1.0f) -
             (amplitude - 1.0f) * cosine - beta;
    }

    return (biquad_coefficients_t) {
        .b0 = b0 / a0,
        .b1 = b1 / a0,
        .b2 = b2 / a0,
        .a1 = a1 / a0,
        .a2 = a2 / a0,
    };
}

static biquad_coefficients_t make_peak(uint32_t sample_rate, float frequency,
                                       float gain_db, float q)
{
    if (sample_rate == 0U || fabsf(gain_db) < 0.0001f) {
        return identity_coefficients();
    }
    float nyquist_limit = (float)sample_rate * 0.45f;
    if (frequency > nyquist_limit) frequency = nyquist_limit;

    float amplitude = powf(10.0f, gain_db / 40.0f);
    float omega = 2.0f * DSP_PI * frequency / (float)sample_rate;
    float cosine = cosf(omega);
    float alpha = sinf(omega) / (2.0f * q);
    float a0 = 1.0f + alpha / amplitude;

    return (biquad_coefficients_t) {
        .b0 = (1.0f + alpha * amplitude) / a0,
        .b1 = (-2.0f * cosine) / a0,
        .b2 = (1.0f - alpha * amplitude) / a0,
        .a1 = (-2.0f * cosine) / a0,
        .a2 = (1.0f - alpha / amplitude) / a0,
    };
}

static biquad_coefficients_t make_lowpass(uint32_t sample_rate,
                                          float frequency, float q)
{
    float omega = 2.0f * DSP_PI * frequency / (float)sample_rate;
    float cosine = cosf(omega);
    float alpha = sinf(omega) / (2.0f * q);
    float a0 = 1.0f + alpha;
    return (biquad_coefficients_t) {
        .b0 = ((1.0f - cosine) * 0.5f) / a0,
        .b1 = (1.0f - cosine) / a0,
        .b2 = ((1.0f - cosine) * 0.5f) / a0,
        .a1 = (-2.0f * cosine) / a0,
        .a2 = (1.0f - alpha) / a0,
    };
}

static biquad_coefficients_t make_highpass(uint32_t sample_rate,
                                           float frequency, float q)
{
    float omega = 2.0f * DSP_PI * frequency / (float)sample_rate;
    float cosine = cosf(omega);
    float alpha = sinf(omega) / (2.0f * q);
    float a0 = 1.0f + alpha;
    return (biquad_coefficients_t) {
        .b0 = ((1.0f + cosine) * 0.5f) / a0,
        .b1 = -(1.0f + cosine) / a0,
        .b2 = ((1.0f + cosine) * 0.5f) / a0,
        .a1 = (-2.0f * cosine) / a0,
        .a2 = (1.0f - alpha) / a0,
    };
}

static float biquad_process(biquad_state_t *state,
                            const biquad_coefficients_t *coefficients,
                            float input)
{
    float output = coefficients->b0 * input + state->z1;
    state->z1 = coefficients->b1 * input -
                coefficients->a1 * output + state->z2;
    state->z2 = coefficients->b2 * input - coefficients->a2 * output;
    return output;
}

static float next_random_unit(void)
{
    uint32_t value = s_dsp.random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    s_dsp.random_state = value;
    return (float)(value >> 8) * (1.0f / 16777216.0f);
}

static float tpdf_dither(void)
{
    return next_random_unit() - next_random_unit();
}

static float update_one_pole(float current, float target, float coefficient)
{
    return current + (target - current) * coefficient;
}

void dsp_init(uint32_t sample_rate)
{
    memset(&s_dsp, 0, sizeof(s_dsp));
    s_dsp.sample_rate = sample_rate > 0U ? sample_rate : 44100U;
    s_dsp.target_volume_gain = 0.25f;
    s_dsp.target_left_balance = 1.0f;
    s_dsp.target_right_balance = 1.0f;
    s_dsp.target_mute_gain = 1.0f;
    s_dsp.volume_gain = s_dsp.target_volume_gain;
    s_dsp.left_balance = 1.0f;
    s_dsp.right_balance = 1.0f;
    s_dsp.mute_gain = 1.0f;
    s_dsp.limiter_gain = 1.0f;
    s_dsp.random_state = 0x6d2b79f5U;
    s_dsp.lookahead_frames = (size_t)((float)s_dsp.sample_rate *
                                      LOOKAHEAD_SECONDS + 0.5f);
    if (s_dsp.lookahead_frames >= LOOKAHEAD_MAX_FRAMES) {
        s_dsp.lookahead_frames = LOOKAHEAD_MAX_FRAMES - 1U;
    }
    s_dsp.gain_ramp = 1.0f - expf(-1.0f /
        ((float)s_dsp.sample_rate * GAIN_RAMP_SECONDS));
    s_dsp.limiter_attack = 1.0f - expf(-1.0f /
        ((float)s_dsp.sample_rate * LIMITER_ATTACK_SECONDS));
    s_dsp.limiter_release = 1.0f - expf(-1.0f /
        ((float)s_dsp.sample_rate * LIMITER_RELEASE_SECONDS));
    s_dsp.vocal_hp = make_highpass(s_dsp.sample_rate, VOCAL_LOW_HZ,
                                   VOCAL_FILTER_Q);
    s_dsp.vocal_lp = make_lowpass(s_dsp.sample_rate, VOCAL_HIGH_HZ,
                                  VOCAL_FILTER_Q);
    s_dsp.bass_eq = identity_coefficients();
    s_dsp.treble_eq = identity_coefficients();
    s_dsp.vocal_eq = identity_coefficients();
}

void dsp_init_muted(uint32_t sample_rate)
{
    dsp_init(sample_rate);
    s_dsp.target_mute_gain = 0.0f;
    s_dsp.mute_gain = 0.0f;
}

void dsp_set_params(float bass_db, float treble_db, float vocal_db,
                    int balance, int volume, bool muted, int vocal_mode)
{
    if (balance < -100) balance = -100;
    if (balance > 100) balance = 100;
    if (volume < 0) volume = 0;
    if (volume > DSP_VOLUME_MAX) volume = DSP_VOLUME_MAX;

    s_dsp.target_bass_db = bass_db;
    s_dsp.target_treble_db = treble_db;
    s_dsp.target_vocal_db = vocal_db;
    float volume_fraction = (float)volume * (1.0f / 100.0f);
    s_dsp.target_volume_gain = volume_fraction * volume_fraction;
    s_dsp.target_left_balance =
        (float)(balance > 0 ? 100 - balance : 100) * 0.01f;
    s_dsp.target_right_balance =
        (float)(balance < 0 ? 100 + balance : 100) * 0.01f;
    s_dsp.target_mute_gain = muted ? 0.0f : 1.0f;
    s_dsp.vocal_mode = vocal_mode;
}

void dsp_process(int16_t *stereo, size_t frames)
{
    if (stereo == NULL || frames == 0U || s_dsp.sample_rate == 0U) {
        return;
    }

    float eq_smoothing = 1.0f - expf(-(float)frames /
        ((float)s_dsp.sample_rate * EQ_GAIN_SMOOTHING_SECONDS));
    s_dsp.bass_db = update_one_pole(s_dsp.bass_db,
                                    s_dsp.target_bass_db, eq_smoothing);
    s_dsp.treble_db = update_one_pole(s_dsp.treble_db,
                                      s_dsp.target_treble_db, eq_smoothing);
    s_dsp.vocal_db = update_one_pole(s_dsp.vocal_db,
                                     s_dsp.target_vocal_db, eq_smoothing);

    if (fabsf(s_dsp.bass_db - s_dsp.coefficient_bass_db) >=
        EQ_RECALC_THRESHOLD_DB) {
        s_dsp.bass_eq = make_shelf(s_dsp.sample_rate, EQ_BASS_HZ,
                                   s_dsp.bass_db, true);
        s_dsp.coefficient_bass_db = s_dsp.bass_db;
    }
    if (fabsf(s_dsp.treble_db - s_dsp.coefficient_treble_db) >=
        EQ_RECALC_THRESHOLD_DB) {
        s_dsp.treble_eq = make_shelf(s_dsp.sample_rate, EQ_TREBLE_HZ,
                                     s_dsp.treble_db, false);
        s_dsp.coefficient_treble_db = s_dsp.treble_db;
    }
    if (fabsf(s_dsp.vocal_db - s_dsp.coefficient_vocal_db) >=
        EQ_RECALC_THRESHOLD_DB) {
        s_dsp.vocal_eq = make_peak(s_dsp.sample_rate, EQ_VOCAL_HZ,
                                   s_dsp.vocal_db, EQ_VOCAL_Q);
        s_dsp.coefficient_vocal_db = s_dsp.vocal_db;
    }

    size_t lookahead_index = s_dsp.lookahead_index;
    size_t delay = s_dsp.lookahead_frames;
    const float threshold = LIMITER_THRESHOLD;

    for (size_t frame = 0; frame < frames; frame++) {
        /* Let the limiter handle real peaks instead of attenuating the whole mix. */
        float left = (float)stereo[frame * 2U];
        float right = (float)stereo[frame * 2U + 1U];
        left = biquad_process(&s_dsp.bass_state[0], &s_dsp.bass_eq, left);
        right = biquad_process(&s_dsp.bass_state[1], &s_dsp.bass_eq, right);
        left = biquad_process(&s_dsp.treble_state[0], &s_dsp.treble_eq, left);
        right = biquad_process(&s_dsp.treble_state[1], &s_dsp.treble_eq, right);
        left = biquad_process(&s_dsp.vocal_eq_state[0], &s_dsp.vocal_eq, left);
        right = biquad_process(&s_dsp.vocal_eq_state[1], &s_dsp.vocal_eq, right);

        if (s_dsp.vocal_mode == VOCAL_MODE_ISOLATE ||
            s_dsp.vocal_mode == VOCAL_MODE_REMOVE) {
            float mid = (left + right) * 0.5f;
            float vocal_band = biquad_process(&s_dsp.vocal_hp_state[0],
                                               &s_dsp.vocal_hp, mid);
            vocal_band = biquad_process(&s_dsp.vocal_hp_state[1],
                                        &s_dsp.vocal_hp, vocal_band);
            vocal_band = biquad_process(&s_dsp.vocal_lp_state[0],
                                        &s_dsp.vocal_lp, vocal_band);
            vocal_band = biquad_process(&s_dsp.vocal_lp_state[1],
                                        &s_dsp.vocal_lp, vocal_band);

            if (s_dsp.vocal_mode == VOCAL_MODE_ISOLATE) {
                left = vocal_band +
                       (left - vocal_band) * VOCAL_BACKGROUND_LEVEL;
                right = vocal_band +
                        (right - vocal_band) * VOCAL_BACKGROUND_LEVEL;
            } else {
                left = (left - vocal_band * VOCAL_REMOVE_AMOUNT) *
                       VOCAL_REMOVE_LEVEL_COMP;
                right = (right - vocal_band * VOCAL_REMOVE_AMOUNT) *
                        VOCAL_REMOVE_LEVEL_COMP;
            }
        }

        s_dsp.volume_gain = update_one_pole(s_dsp.volume_gain,
            s_dsp.target_volume_gain, s_dsp.gain_ramp);
        s_dsp.left_balance = update_one_pole(s_dsp.left_balance,
            s_dsp.target_left_balance, s_dsp.gain_ramp);
        s_dsp.right_balance = update_one_pole(s_dsp.right_balance,
            s_dsp.target_right_balance, s_dsp.gain_ramp);
        s_dsp.mute_gain = update_one_pole(s_dsp.mute_gain,
            s_dsp.target_mute_gain, s_dsp.gain_ramp);
        float gain = s_dsp.volume_gain * s_dsp.mute_gain;
        left *= gain * s_dsp.left_balance;
        right *= gain * s_dsp.right_balance;

        float delayed_left = s_dsp.lookahead[lookahead_index][0];
        float delayed_right = s_dsp.lookahead[lookahead_index][1];
        s_dsp.lookahead[lookahead_index][0] = left;
        s_dsp.lookahead[lookahead_index][1] = right;
        lookahead_index++;
        if (lookahead_index >= LOOKAHEAD_MAX_FRAMES) lookahead_index = 0U;
        if (delay == 0U) {
            delayed_left = left;
            delayed_right = right;
        } else {
            size_t read_index = lookahead_index + LOOKAHEAD_MAX_FRAMES -
                                delay - 1U;
            if (read_index >= LOOKAHEAD_MAX_FRAMES) {
                read_index -= LOOKAHEAD_MAX_FRAMES;
            }
            delayed_left = s_dsp.lookahead[read_index][0];
            delayed_right = s_dsp.lookahead[read_index][1];
        }

        float peak = fmaxf(fabsf(left), fabsf(right));
        float target_limiter_gain = peak > threshold ? threshold / peak : 1.0f;
        float limiter_coefficient = target_limiter_gain < s_dsp.limiter_gain
            ? s_dsp.limiter_attack : s_dsp.limiter_release;
        s_dsp.limiter_gain = update_one_pole(s_dsp.limiter_gain,
            target_limiter_gain, limiter_coefficient);
        float delayed_peak = fmaxf(fabsf(delayed_left), fabsf(delayed_right));
        float applied_gain = s_dsp.limiter_gain;
        if (delayed_peak > threshold && threshold / delayed_peak < applied_gain) {
            applied_gain = threshold / delayed_peak;
        }
        delayed_left *= applied_gain;
        delayed_right *= applied_gain;

        float dither_left = s_dsp.mute_gain < 0.0001f ? 0.0f : tpdf_dither();
        float dither_right = s_dsp.mute_gain < 0.0001f ? 0.0f : tpdf_dither();
        float rounded_left = floorf(delayed_left + dither_left + 0.5f);
        float rounded_right = floorf(delayed_right + dither_right + 0.5f);
        if (rounded_left > threshold) rounded_left = threshold;
        if (rounded_left < -threshold) rounded_left = -threshold;
        if (rounded_right > threshold) rounded_right = threshold;
        if (rounded_right < -threshold) rounded_right = -threshold;
        stereo[frame * 2U] = (int16_t)rounded_left;
        stereo[frame * 2U + 1U] = (int16_t)rounded_right;
    }
    s_dsp.lookahead_index = lookahead_index;
}
