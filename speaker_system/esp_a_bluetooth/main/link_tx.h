#ifndef SPEAKER_LINK_TX_H
#define SPEAKER_LINK_TX_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t link_tx_init(void);
esp_err_t link_tx_send_rate(uint32_t sample_rate);

#endif
