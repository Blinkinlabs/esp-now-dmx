//! ESP-NOW DMX data transceiver example
//!
//! To use, compile with 'ROLE_SENDER' defined and flash to one device, then
//! compile with 'ROLE_SENDER' undefined and flash to one or more devices.

#include <string.h>
#include <esp_log.h>
#include <esp_err.h>
#include <math.h>

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include "espnow_transponder.h"

#define UNIVERSE_COUNT 20
#define FRAMERATE 44

#define ROLE_SENDER

#if defined(ROLE_SENDER)
static const char *TAG = "espnow_tx";
#else
static const char *TAG = "espnow_rx";
#endif

//! Data structure for an ARTDMX packet
//!
//! Note: Unfortunately ESP-NOW packets have a maximum length of 250, so they
//!       can send a little less than half of a full DMX512 network. This
//!       implementation just discards any data that doesn't fit. A more
//!       complete implementation might add an offset field, and fragment the
//!       Art-Net packet into several ESP-NOW packets.
typedef struct {
    uint16_t universe;                  //!< DMX universe for this data
    uint8_t sequence;                   //!< Sequence number
    uint8_t data[];                     //!< DMX data
} __attribute__((packed)) artdmx_packet_t;

typedef struct {
    uint32_t count;
    uint32_t oos;
    uint8_t last_sequence;
} universe_stats_t;

universe_stats_t universe_stats[UNIVERSE_COUNT];

void universe_stats_print() {
    for(int universe = 0; universe < UNIVERSE_COUNT; universe++) {
        const universe_stats_t *stat = &(universe_stats[universe]);
        ESP_LOGI(TAG, "universe:%2i packets:%i oos:%i", universe, stat->count, stat->oos);
    }
}

void universe_stats_init() {
    for(int universe = 0; universe < UNIVERSE_COUNT; universe++) {
        universe_stats_t *stat = &(universe_stats[universe]);
        stat->count = 0;
        stat->oos = 0;
        stat->last_sequence = 255;
    }
}

void universe_stats_record(uint16_t universe, uint8_t sequence) {
    if(universe >= UNIVERSE_COUNT)
        return;

    universe_stats_t *stat = &(universe_stats[universe]);
    stat->count++;
    if(sequence != ((stat->last_sequence + 1)%256))
        stat->oos++;

    stat->last_sequence = sequence;
}

//! @brief Broadcast data to the specified universe
//!
//! \param universe Art-Net universe to send to
//! \param sequence Sequence number
//! \param data Pointer to the data
//! \param data_len Length of the data
void send_artdmx_packet(uint16_t universe, uint8_t sequence, const uint8_t *data, uint8_t data_length) {
    // Wrap the data into a payload with this structure:
    // payload[0-1]: universe
    // payload[2]: sequence
    // payload[3-n]: data
    // The next higher layer will guarantee data length and CRC, so they are not needed here.

    const int packet_len = data_length + sizeof(artdmx_packet_t);
    uint8_t packet_buffer[packet_len];

    artdmx_packet_t *header = (artdmx_packet_t *)packet_buffer;
    header->universe = universe;
    header->sequence = sequence;
    memcpy(header->data, data, data_length);

    //ESP_LOGI(TAG, "uni:%i, len:%i d[238]:%02x d[239]:%02x", universe, data_length, data[238], data[239]);

    esp_err_t ret = espnow_transponder_send(packet_buffer, packet_len);
    if(ret != ESP_OK)
        ESP_LOGE(TAG, "Send error, err=%s", esp_err_to_name(ret));
}

void receive_packet(const uint8_t *data, uint8_t data_length) {
    artdmx_packet_t *header = (artdmx_packet_t *)data;
    universe_stats_record(header->universe, header->sequence);
}

//! \brief Send test packets at a specified framerate
void transmitter_test() {
    const uint32_t framedelay_ms = (1000/FRAMERATE);

    ESP_LOGI(TAG, "Starting sender mode...");

    // Red-to-blue fade
    const uint8_t universe_size = 240;

    uint8_t *buffer = malloc(universe_size*UNIVERSE_COUNT);
    if(buffer == NULL) {
        ESP_LOGE(TAG, "Could not allocate memory for buffer");
        return;
    }

    uint8_t sequence = 0;
    float phase = 0;
    while(true) {
        for(int led = 0; led<(universe_size*UNIVERSE_COUNT)/3; led++) {
            buffer[led*3+0] = (int)(30*(sin(phase+led/100.0)+1));
            buffer[led*3+1] = 0;
            buffer[led*3+2] = 0;
        }

        for(int universe = 0; universe < UNIVERSE_COUNT; universe++)
            send_artdmx_packet(universe, sequence, buffer+(universe*universe_size), universe_size);

        phase += .2;
        sequence++;

//        vTaskDelay(framedelay_ms/portTICK_RATE_MS);
    }
}

//! \brief Listen for packets, and report on their status periodically
void receiver_test() {
    ESP_LOGI(TAG, "Starting receiver mode");
    while(true) {
        vTaskDelay(1000/portTICK_RATE_MS);
        universe_stats_print();
    }
}

void app_main()
{
    universe_stats_init();

    espnow_transponder_init(&espnow_transponder_config_default);
    espnow_transponder_register_callback(receive_packet);

#if defined(ROLE_SENDER)
    transmitter_test();

#else
    receiver_test();
#endif
}
