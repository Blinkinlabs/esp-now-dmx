//! ESP-NOW DMX data transceiver example
//!
//! To use, compile with 'ROLE_SENDER' defined and flash to one device, then
//! compile with 'ROLE_SENDER' undefined and flash to one or more devices.

#include <string.h>
#include <esp_log.h>
#include <esp_err.h>

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include "espnow_transponder.h"

#define UNIVERSE_COUNT 15
//#define ROLE_SENDER

#if defined(ROLE_SENDER)
static const char *TAG = "espnow_tx";
#else
static const char *TAG = "espnow_rx";
#endif

/* User defined field of ESPNOW data in this example. */
typedef struct {
    uint16_t universe;
    uint8_t sequence;
    uint8_t data[0];
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
    // The next higher layer will guarintee data length and CRC, so they are not needed here.

    const int packet_len = data_length + sizeof(artdmx_packet_t) - 1;
    uint8_t packet_buffer[packet_len];

    artdmx_packet_t *header = (artdmx_packet_t *)packet_buffer;
    header->universe = universe;
    header->sequence = sequence;
    memcpy(header->data, data, data_length);

    if(espnow_transponder_send(packet_buffer, packet_len) != ESP_OK) {
        ESP_LOGE(TAG, "Send error");
    }
}

void receive_packet(const uint8_t *data, uint8_t data_length) {
    artdmx_packet_t *header = (artdmx_packet_t *)data;
    universe_stats_record(header->universe, header->sequence);
}

void app_main()
{
    universe_stats_init();

    espnow_transponder_init();
    espnow_transponder_register_callback(receive_packet);

#if defined(ROLE_SENDER)
    const uint32_t framedelay_ms = (1000/30);

    ESP_LOGI(TAG, "Starting sender mode...");

    // Red-to-blue fade
    uint8_t data[8*8*3];

    for(int led = 0; led<sizeof(data)/3; led++) {
        data[led*3+0] = led;
        data[led*3+1] = 0;
        data[led*3+2] = (sizeof(data)/3)-led;
    }

    uint8_t sequence = 0;
    while(true) {
        vTaskDelay(framedelay_ms/portTICK_RATE_MS);

        for(int universe = 0; universe < 10; universe++)
            send_artdmx_packet(universe, sequence, data, sizeof(data));

        sequence++;
    }
#else
    ESP_LOGI(TAG, "Starting receiver mode");
    while(true) {
        vTaskDelay(1000/portTICK_RATE_MS);
        universe_stats_print();
    }
#endif
}
