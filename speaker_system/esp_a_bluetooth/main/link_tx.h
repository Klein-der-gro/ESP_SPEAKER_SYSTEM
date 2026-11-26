#ifndef SPEAKER_LINK_TX_H
#define SPEAKER_LINK_TX_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "protocol.h"

esp_err_t link_tx_init(void);
esp_err_t link_tx_send_rate(uint32_t sample_rate);
esp_err_t link_tx_send_dsp_params(const protocol_dsp_params_t *params);
bool link_tx_get_box_status(uint8_t *status);

#endif
