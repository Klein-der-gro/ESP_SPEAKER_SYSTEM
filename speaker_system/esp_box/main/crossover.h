#ifndef ESP_BOX_CROSSOVER_H
#define ESP_BOX_CROSSOVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
} crossover_biquad_t;

typedef struct {
    float z1;
    float z2;
} crossover_biquad_state_t;

typedef struct {
    crossover_biquad_t low;
    crossover_biquad_t high;
    crossover_biquad_state_t low_state[2];
    crossover_biquad_state_t high_state[2];
    float low_gain;
    float high_gain;
    float low_limiter_gain;
    float high_limiter_gain;
    float mute_gain;
    float mute_target;
    float limiter_attack;
    float limiter_release;
    float mute_ramp;
    float low_delay[2][128];
    float high_delay[2][128];
    size_t delay_index;
    size_t low_delay_samples;
    size_t high_delay_samples;
    bool invert_high;
} crossover_t;

bool crossover_init(crossover_t *state, uint32_t sample_rate);
void crossover_set_muted(crossover_t *state, bool muted);
void crossover_process(crossover_t *state, const int16_t *input,
                       int16_t *stereo_output, size_t frames);

#endif
