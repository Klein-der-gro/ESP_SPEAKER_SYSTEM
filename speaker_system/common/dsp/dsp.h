#ifndef BT_DAC_DSP_H
#define BT_DAC_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DSP_VOLUME_MAX 250

void dsp_init(uint32_t sample_rate);
void dsp_init_muted(uint32_t sample_rate);
void dsp_set_params(float bass_db, float treble_db, float vocal_db,
                    int balance, int volume, bool muted, int vocal_mode);
void dsp_process(int16_t *stereo, size_t frames);

#endif
