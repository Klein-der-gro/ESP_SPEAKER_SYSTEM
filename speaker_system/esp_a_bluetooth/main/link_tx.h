#ifndef SPEAKER_LINK_TX_H
#define SPEAKER_LINK_TX_H

#include <stdint.h>

#include "esp_err.h"
#include "protocol.h"

esp_err_t link_tx_init(void);
esp_err_t link_tx_send_rate(uint32_t sample_rate);
esp_err_t link_tx_send_dsp_params(const protocol_dsp_params_t *params);

#endif
