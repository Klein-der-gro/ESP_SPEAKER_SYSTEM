#ifndef SPEAKER_SYSTEM_PROTOCOL_H
#define SPEAKER_SYSTEM_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FIRMWARE_VERSION_NAME "NR-5.0.26.11"
#define PROTOCOL_FIRMWARE_VERSION UINT32_C(2)

#define PROTOCOL_MAGIC UINT32_C(0x314B5053)
#define PROTOCOL_WIFI_SSID "SPEAKER-NET"
#define PROTOCOL_WIFI_PASSWORD "change-me-1234"
#define PROTOCOL_WIFI_CHANNEL 1
#define PROTOCOL_WIFI_COUNTRY "AT"
#define PROTOCOL_AP_IPV4 "192.168.4.1"

#define PROTOCOL_AUDIO_PORT 5004
#define PROTOCOL_CONTROL_PORT 5005

#define PROTOCOL_TYPE_AUDIO 1
#define PROTOCOL_TYPE_HELLO 2
#define PROTOCOL_TYPE_HELLO_ACK 3
#define PROTOCOL_TYPE_SYNC_REQ 4
#define PROTOCOL_TYPE_SYNC_RESP 5

#define PROTOCOL_FLAG_SILENCE 0x01
#define PROTOCOL_FLAG_STREAM_RESET 0x02

#define PROTOCOL_AUDIO_PAYLOAD_BYTES 1024
#define PROTOCOL_CHUNK_FRAMES 256
#define PROTOCOL_AUDIO_PACKET_BYTES 1056
#define PROTOCOL_PLAYOUT_DELAY_US UINT32_C(120000)
#define PROTOCOL_CROSSOVER_HZ 2500
#define PROTOCOL_CROSSOVER_MIN_HZ 1500
#define DSP_ON_HUB 1

#define PROTOCOL_UART_BAUD 115200
#define PROTOCOL_UART_START_BYTE 0xA5
#define PROTOCOL_UART_TYPE_RATE 1
#define PROTOCOL_UART_TYPE_DSP_PARAMS 2
#define PROTOCOL_UART_TYPE_BOX_STATUS 3
#define PROTOCOL_UART_RATE_PAYLOAD_SIZE 4
#define PROTOCOL_UART_DSP_PARAMS_PAYLOAD_SIZE 18
#define PROTOCOL_UART_BOX_STATUS_PAYLOAD_SIZE 1
#define PROTOCOL_UART_BOX_LEFT_CONNECTED 0x01
#define PROTOCOL_UART_BOX_RIGHT_CONNECTED 0x02
#define PROTOCOL_UART_BOX_STATUS_TIMEOUT_MS 3000

#define PROTOCOL_ROLE_LEFT 0
#define PROTOCOL_ROLE_RIGHT 1
#define PROTOCOL_HEARTBEAT_MS 2000
#define PROTOCOL_UNREGISTERED_HEARTBEAT_MS 1000
#define PROTOCOL_BOX_TIMEOUT_MS 6000

#define PROTOCOL_PACKED __attribute__((packed))

typedef struct PROTOCOL_PACKED {
    uint32_t magic;
    uint8_t type;
    uint8_t flags;
    uint16_t payload_len;
    uint32_t seq;
    uint32_t sample_rate;
    uint64_t frame_index;
    uint64_t hub_time_us;
} protocol_audio_header_t;

typedef struct PROTOCOL_PACKED {
    uint32_t magic;
    uint8_t type;
    uint8_t role;
    uint16_t reserved;
    uint32_t firmware_version;
} protocol_hello_t;

typedef struct PROTOCOL_PACKED {
    float bass_db;
    float treble_db;
    float vocal_db;
    int16_t balance;
    int16_t volume;
    uint8_t muted;
    uint8_t vocal_mode;
} protocol_dsp_params_t;

typedef struct PROTOCOL_PACKED {
    uint32_t magic;
    uint8_t type;
    uint8_t reserved[3];
} protocol_hello_ack_t;

typedef struct PROTOCOL_PACKED {
    uint32_t magic;
    uint8_t type;
    uint8_t reserved[3];
    uint64_t t1;
} protocol_sync_req_t;

typedef struct PROTOCOL_PACKED {
    uint32_t magic;
    uint8_t type;
    uint8_t reserved[3];
    uint64_t t1;
    uint64_t t2;
    uint64_t t3;
} protocol_sync_resp_t;

_Static_assert(sizeof(protocol_audio_header_t) == 32,
               "audio packet header must be 32 bytes");
_Static_assert(sizeof(protocol_dsp_params_t) ==
                   PROTOCOL_UART_DSP_PARAMS_PAYLOAD_SIZE,
               "UART DSP parameter payload must be 18 bytes");

static inline uint8_t protocol_crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80U) != 0U
                ? (uint8_t)((crc << 1) ^ 0x07U)
                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

#endif
