#include "crossover.h"

#include <math.h>
#include <string.h>

#include "protocol.h"

#define CROSSOVER_PI 3.14159265358979323846f
#define CROSSOVER_Q 0.70710678118f
#define CROSSOVER_LOW_TRIM_DB 0.0f
#define CROSSOVER_HIGH_TRIM_DB 0.0f
#define CROSSOVER_INVERT_HIGH 0
#define CROSSOVER_LOW_DELAY_SAMPLES 0U
#define CROSSOVER_HIGH_DELAY_SAMPLES 0U
#define CROSSOVER_LIMITER_CEILING 29203.0f
#define CROSSOVER_LIMITER_ATTACK_SECONDS 0.001f
#define CROSSOVER_LIMITER_RELEASE_SECONDS 0.080f
#define CROSSOVER_MUTE_RAMP_SECONDS 0.010f

static crossover_biquad_t make_filter(uint32_t sample_rate, float frequency,
                                      bool highpass)
{
    float omega = 2.0f * CROSSOVER_PI * frequency / (float)sample_rate;
    float cosine = cosf(omega);
    float alpha = sinf(omega) / (2.0f * CROSSOVER_Q);
    float a0 = 1.0f + alpha;
    if (highpass) {
        return (crossover_biquad_t) {
            .b0 = ((1.0f + cosine) * 0.5f) / a0,
            .b1 = -(1.0f + cosine) / a0,
            .b2 = ((1.0f + cosine) * 0.5f) / a0,
            .a1 = (-2.0f * cosine) / a0,
            .a2 = (1.0f - alpha) / a0,
        };
    }
    return (crossover_biquad_t) {
        .b0 = ((1.0f - cosine) * 0.5f) / a0,
        .b1 = (1.0f - cosine) / a0,
        .b2 = ((1.0f - cosine) * 0.5f) / a0,
        .a1 = (-2.0f * cosine) / a0,
        .a2 = (1.0f - alpha) / a0,
    };
}

static float process_biquad(crossover_biquad_state_t *state,
                            const crossover_biquad_t *filter, float input)
{
    float output = filter->b0 * input + state->z1;
    state->z1 = filter->b1 * input - filter->a1 * output + state->z2;
    state->z2 = filter->b2 * input - filter->a2 * output;
    return output;
}

bool crossover_init(crossover_t *state, uint32_t sample_rate)
{
    if (state == NULL || sample_rate == 0U ||
        sample_rate <= PROTOCOL_CROSSOVER_MIN_HZ * 2U ||
        PROTOCOL_CROSSOVER_HZ < PROTOCOL_CROSSOVER_MIN_HZ ||
        PROTOCOL_CROSSOVER_HZ >= sample_rate / 2U ||
        CROSSOVER_LOW_DELAY_SAMPLES > 128U ||
        CROSSOVER_HIGH_DELAY_SAMPLES > 128U) {
        return false;
    }
    memset(state, 0, sizeof(*state));
    state->low = make_filter(sample_rate, PROTOCOL_CROSSOVER_HZ, false);
    state->high = make_filter(sample_rate, PROTOCOL_CROSSOVER_HZ, true);
    state->low_gain = powf(10.0f, CROSSOVER_LOW_TRIM_DB / 20.0f);
    state->high_gain = powf(10.0f, CROSSOVER_HIGH_TRIM_DB / 20.0f);
    state->low_limiter_gain = 1.0f;
    state->high_limiter_gain = 1.0f;
    state->mute_target = 0.0f;
    state->limiter_attack = 1.0f - expf(-1.0f /
        ((float)sample_rate * CROSSOVER_LIMITER_ATTACK_SECONDS));
    state->limiter_release = 1.0f - expf(-1.0f /
        ((float)sample_rate * CROSSOVER_LIMITER_RELEASE_SECONDS));
    state->mute_ramp = 1.0f /
        ((float)sample_rate * CROSSOVER_MUTE_RAMP_SECONDS);
    state->low_delay_samples = CROSSOVER_LOW_DELAY_SAMPLES;
    state->high_delay_samples = CROSSOVER_HIGH_DELAY_SAMPLES;
    state->invert_high = CROSSOVER_INVERT_HIGH != 0;
    return true;
}

void crossover_set_muted(crossover_t *state, bool muted)
{
    if (state != NULL) {
        state->mute_target = muted ? 0.0f : 1.0f;
    }
}

static float limit_path(float sample, float *gain, float attack,
                        float release)
{
    float magnitude = fabsf(sample);
    float target = magnitude > CROSSOVER_LIMITER_CEILING
        ? CROSSOVER_LIMITER_CEILING / magnitude : 1.0f;
    float coefficient = target < *gain ? attack : release;
    *gain += (target - *gain) * coefficient;
    float output = sample * *gain;
    if (output > CROSSOVER_LIMITER_CEILING) {
        output = CROSSOVER_LIMITER_CEILING;
    } else if (output < -CROSSOVER_LIMITER_CEILING) {
        output = -CROSSOVER_LIMITER_CEILING;
    }
    return output;
}

void crossover_process(crossover_t *state, const int16_t *input,
                       int16_t *stereo_output, size_t frames)
{
    if (state == NULL || input == NULL || stereo_output == NULL) {
        return;
    }
    for (size_t frame = 0; frame < frames; frame++) {
        float sample = (float)input[frame];
        float low = process_biquad(&state->low_state[0], &state->low, sample);
        low = process_biquad(&state->low_state[1], &state->low, low);
        float high = process_biquad(&state->high_state[0], &state->high, sample);
        high = process_biquad(&state->high_state[1], &state->high, high);

        size_t delay_slot = state->delay_index & 127U;
        if (state->low_delay_samples > 0U) {
            float delayed = state->low_delay[0][
                (delay_slot + 128U - state->low_delay_samples) & 127U];
            state->low_delay[0][delay_slot] = low;
            low = delayed;
        }
        if (state->high_delay_samples > 0U) {
            float delayed = state->high_delay[1][
                (delay_slot + 128U - state->high_delay_samples) & 127U];
            state->high_delay[1][delay_slot] = high;
            high = delayed;
        }
        state->delay_index++;

        float mute_delta = state->mute_target - state->mute_gain;
        if (fabsf(mute_delta) <= state->mute_ramp) {
            state->mute_gain = state->mute_target;
        } else {
            state->mute_gain += copysignf(state->mute_ramp, mute_delta);
        }
        low = limit_path(low * state->low_gain * state->mute_gain,
                         &state->low_limiter_gain, state->limiter_attack,
                         state->limiter_release);
        high = limit_path(high * state->high_gain * state->mute_gain,
                          &state->high_limiter_gain, state->limiter_attack,
                          state->limiter_release);
        if (state->invert_high) high = -high;
        stereo_output[frame * 2U] = (int16_t)lrintf(high);
        stereo_output[frame * 2U + 1U] = (int16_t)lrintf(low);
    }
}
