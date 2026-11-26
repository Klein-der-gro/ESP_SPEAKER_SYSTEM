#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsp.h"

#define TEST_FRAMES 256U
#define TEST_CHUNKS 32U
#define TEST_PI 3.14159265358979323846f

static void fill_tone(int16_t *samples, uint32_t first_frame,
                      uint32_t frames, uint32_t sample_rate,
                      float frequency, float amplitude)
{
    for (uint32_t frame = 0; frame < frames; frame++) {
        float phase = 2.0f * TEST_PI * frequency *
                      (float)(first_frame + frame) / (float)sample_rate;
        int16_t sample = (int16_t)lrintf(sinf(phase) * amplitude);
        samples[frame * 2U] = sample;
        samples[frame * 2U + 1U] = sample;
    }
}

static uint32_t maximum_magnitude(const int16_t *samples, size_t count)
{
    uint32_t maximum = 0U;
    for (size_t i = 0; i < count; i++) {
        int32_t value = samples[i];
        uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        if (magnitude > maximum) maximum = magnitude;
    }
    return maximum;
}

int main(void)
{
    int16_t samples[TEST_FRAMES * 2U];
    dsp_init_muted(44100U);
    dsp_set_params(0.0f, 0.0f, 0.0f, 0, 100, true, 0);
    fill_tone(samples, 0U, TEST_FRAMES, 44100U, 1000.0f, 12000.0f);
    dsp_process(samples, TEST_FRAMES);
    if (maximum_magnitude(samples, TEST_FRAMES * 2U) != 0U) {
        fprintf(stderr, "FAIL: DSP start-muted output was not silent\n");
        return EXIT_FAILURE;
    }

    dsp_set_params(0.0f, 0.0f, 0.0f, 0, 100, false, 0);
    uint32_t frame_index = 0U;
    uint32_t unmuted_peak = 0U;
    for (uint32_t chunk = 0; chunk < TEST_CHUNKS; chunk++) {
        fill_tone(samples, frame_index, TEST_FRAMES, 44100U,
                  1000.0f, 12000.0f);
        frame_index += TEST_FRAMES;
        dsp_process(samples, TEST_FRAMES);
        uint32_t peak = maximum_magnitude(samples, TEST_FRAMES * 2U);
        if (peak > unmuted_peak) unmuted_peak = peak;
        if (peak > 26028U) {
            fprintf(stderr, "FAIL: limiter exceeded its ceiling\n");
            return EXIT_FAILURE;
        }
    }
    if (unmuted_peak < 1000U) {
        fprintf(stderr, "FAIL: DSP did not unmute after valid parameters\n");
        return EXIT_FAILURE;
    }

    dsp_set_params(0.0f, 0.0f, 0.0f, 0, 100, true, 0);
    uint32_t muted_peak = 0U;
    for (uint32_t chunk = 0; chunk < TEST_CHUNKS; chunk++) {
        fill_tone(samples, frame_index, TEST_FRAMES, 44100U,
                  1000.0f, 12000.0f);
        frame_index += TEST_FRAMES;
        dsp_process(samples, TEST_FRAMES);
        uint32_t peak = maximum_magnitude(samples, TEST_FRAMES * 2U);
        if (peak > muted_peak) muted_peak = peak;
    }
    if (muted_peak > 2U) {
        fprintf(stderr, "FAIL: DSP mute did not ramp to silence\n");
        return EXIT_FAILURE;
    }

    puts("PASS: DSP starts muted, unmutes on control, limits peaks, and mutes again");
    return EXIT_SUCCESS;
}
