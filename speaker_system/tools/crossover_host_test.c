#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "crossover.h"
#include "protocol.h"

#define TEST_SAMPLE_RATE 44100U
#define TEST_AMPLITUDE 12000.0f
#define TEST_WARMUP_FRAMES 4096U
#define TEST_MEASURE_FRAMES 4096U
#define TEST_PI 3.14159265358979323846f

typedef struct {
    float low;
    float high;
    float sum;
} measurement_t;

static measurement_t measure_frequency(float frequency)
{
    crossover_t crossover;
    if (!crossover_init(&crossover, TEST_SAMPLE_RATE)) {
        fprintf(stderr, "FAIL: crossover init rejected sample rate\n");
        exit(EXIT_FAILURE);
    }
    crossover_set_muted(&crossover, false);

    int16_t input[TEST_MEASURE_FRAMES];
    int16_t output[TEST_MEASURE_FRAMES * 2U];
    measurement_t result = {0};
    uint64_t low_energy = 0U;
    uint64_t high_energy = 0U;
    uint64_t sum_energy = 0U;
    uint32_t frame_index = 0U;
    uint32_t total_frames = TEST_WARMUP_FRAMES + TEST_MEASURE_FRAMES;
    while (frame_index < total_frames) {
        size_t frames = total_frames - frame_index;
        if (frames > TEST_MEASURE_FRAMES) frames = TEST_MEASURE_FRAMES;
        for (size_t i = 0; i < frames; i++) {
            float phase = 2.0f * TEST_PI * frequency *
                          (float)(frame_index + i) / (float)TEST_SAMPLE_RATE;
            input[i] = (int16_t)lrintf(sinf(phase) * TEST_AMPLITUDE);
        }
        crossover_process(&crossover, input, output, frames);
        if (frame_index + frames > TEST_WARMUP_FRAMES) {
            size_t start = frame_index < TEST_WARMUP_FRAMES
                ? TEST_WARMUP_FRAMES - frame_index : 0U;
            for (size_t i = start; i < frames; i++) {
                int32_t high = output[i * 2U];
                int32_t low = output[i * 2U + 1U];
                int32_t sum = high + low;
                low_energy += (uint64_t)(low * low);
                high_energy += (uint64_t)(high * high);
                sum_energy += (uint64_t)(sum * sum);
                if (abs(high) > 29204 || abs(low) > 29204) {
                    fprintf(stderr, "FAIL: crossover output exceeded limiter ceiling\n");
                    exit(EXIT_FAILURE);
                }
            }
        }
        frame_index += (uint32_t)frames;
    }
    float count = (float)TEST_MEASURE_FRAMES;
    result.low = sqrtf((float)low_energy / count) * 1.41421356f /
                 TEST_AMPLITUDE;
    result.high = sqrtf((float)high_energy / count) * 1.41421356f /
                  TEST_AMPLITUDE;
    result.sum = sqrtf((float)sum_energy / count) * 1.41421356f /
                 TEST_AMPLITUDE;
    return result;
}

static float db(float amplitude)
{
    return 20.0f * log10f(amplitude);
}

int main(void)
{
    const float frequencies[] = {250.0f, 500.0f, 1000.0f,
                                 2500.0f, 5000.0f, 10000.0f};
    for (size_t i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]); i++) {
        measurement_t response = measure_frequency(frequencies[i]);
        if (fabsf(db(response.sum)) > 0.5f) {
            fprintf(stderr, "FAIL: summed response at %.0f Hz is %.2f dB\n",
                    frequencies[i], db(response.sum));
            return EXIT_FAILURE;
        }
    }

    measurement_t at_crossover =
        measure_frequency((float)PROTOCOL_CROSSOVER_HZ);
    if (fabsf(db(at_crossover.low) + 6.02f) > 0.35f ||
        fabsf(db(at_crossover.high) + 6.02f) > 0.35f) {
        fprintf(stderr, "FAIL: crossover paths are not -6 dB at crossover\n");
        return EXIT_FAILURE;
    }

    measurement_t low_half =
        measure_frequency((float)PROTOCOL_CROSSOVER_HZ / 2.0f);
    measurement_t low_double =
        measure_frequency((float)PROTOCOL_CROSSOVER_HZ * 2.0f);
    measurement_t high_half =
        measure_frequency((float)PROTOCOL_CROSSOVER_HZ / 2.0f);
    measurement_t high_double =
        measure_frequency((float)PROTOCOL_CROSSOVER_HZ * 2.0f);
    float low_slope = db(low_double.low) - db(low_half.low);
    float high_slope = db(high_half.high) - db(high_double.high);
    if (low_slope > -22.0f || high_slope > -22.0f) {
        fprintf(stderr, "FAIL: LR4 rejection per two octaves too low (%.2f/%.2f dB)\n",
                low_slope, high_slope);
        return EXIT_FAILURE;
    }
    puts("PASS: LR4 crossover response, summation, and per-way limiting");
    return EXIT_SUCCESS;
}
