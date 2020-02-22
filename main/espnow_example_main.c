/* ESPNOW Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
   This example shows how to use ESPNOW.
   Prepare two device, one for sending ESPNOW data and another for receiving
   ESPNOW data.
*/
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "tcpip_adapter.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_now.h"
#include "rom/ets_sys.h"
#include "rom/crc.h"
#include "espnow_example.h"

#include "esp_wifi_internal.h"

#define ROLE_SENDER

#if defined(ROLE_SENDER)
static const char *TAG = "espnow_tx";
#else
static const char *TAG = "espnow_rx";
#endif

#define UNIVERSE_COUNT 15

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

static xQueueHandle s_example_espnow_queue;

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

//static void example_espnow_deinit(example_espnow_send_param_t *send_param);

static esp_err_t example_event_handler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAG, "WiFi started");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* WiFi should start before using ESPNOW */
static void example_wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK( esp_event_loop_init(example_event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());

    /* In order to simplify example, channel is set after WiFi started.
     * This is not necessary in real application if the two devices have
     * been already on the same channel.
     */
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, 0) );


    // From: https://www.esp32.com/viewtopic.php?t=9965
    // Attempt to change the wifi modulation speed
//    esp_wifi_internal_set_fix_rate(ESPNOW_WIFI_IF, 1, WIFI_PHY_RATE_MCS7_SGI);


#if CONFIG_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_example_espnow_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void example_espnow_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_example_espnow_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

//! @brief Parts an incoming ESP-NOW packet
//!
//! \param data Pointer to the data packet
//! \param data_len Length of the data packet
//! \return True if the packet passed CRC check
bool parse_packet(uint8_t *packet, uint16_t packet_length)
{
    // Check the the packet is the bare minimum size
    if (packet_length < sizeof(example_espnow_data_t) + 1) {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", packet_length);
        return false;
    }

    example_espnow_data_t *header = (example_espnow_data_t *)packet;

    // Check the CRC
    const uint16_t crc = header->crc;
    header->crc = 0;    // Feed this back so we don't have to overwrite it
    const uint16_t crc_cal = crc16_le(UINT16_MAX, packet, packet_length);
    if(crc_cal != crc) {
        ESP_LOGE(TAG, "Failed CRC check, expected:%04x got:%04x", crc, crc_cal);
        return false;
    }

    const uint16_t expected_length = header->data_length + sizeof(example_espnow_data_t) - 1;
    if(expected_length != packet_length) {
        ESP_LOGE(TAG, "Invalid length, expected:%i got:%i", expected_length, packet_length);
        return false;
    }

    return true;
}

//! @brief TX/RX callback handler task
static void example_espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;

    while (xQueueReceive(s_example_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case EXAMPLE_ESPNOW_SEND_CB:
            {
                example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                ESP_LOGD(TAG, "Sent data to "MACSTR", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);
                break;
            }
            case EXAMPLE_ESPNOW_RECV_CB:
            {
                example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                const bool ret = parse_packet(recv_cb->data, recv_cb->data_len);
                if(ret == true) {
                    example_espnow_data_t *header = (example_espnow_data_t *)recv_cb->data;
                    //ESP_LOGI(TAG, "RX universe:%i seq:%i len:%i", header->universe, header->seq_num, header->data_length);
                    universe_stats_record(header->universe, header->seq_num);
                }
                free(recv_cb->data);
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", evt.id);
                break;
        }
    }
}

//! @brief Initialize the ESP-NOW module
static esp_err_t example_espnow_init(void)
{
    s_example_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_example_espnow_queue == NULL) {
        ESP_LOGE(TAG, "Create queue fail");
        return ESP_FAIL;
    }

    // Initialize ESPNOW and register sending and receiving callback function.
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(example_espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(example_espnow_recv_cb) );

    // Set primary master key.
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    // Add broadcast peer information to peer list.
    esp_now_peer_info_t peer = {
        .channel = CONFIG_ESPNOW_CHANNEL,
        .ifidx = ESPNOW_WIFI_IF,
        .encrypt = false,
    };
    memcpy(&(peer.peer_addr), s_example_broadcast_mac, sizeof(peer.peer_addr));
    ESP_ERROR_CHECK( esp_now_add_peer(&peer) );

    xTaskCreate(example_espnow_task, "example_espnow_task", 2048, NULL, 4, NULL);

    return ESP_OK;
}


//! @brief Broadcast data to the specified universe
//!
//! \param universe Art-Net universe to send to
//! \param sequence Sequence number
//! \param data Pointer to the data
//! \param data_len Length of the data
void send_universe_data(uint16_t universe, uint8_t sequence, void *data, uint8_t data_length) {

    const int packet_len = data_length + sizeof(example_espnow_data_t) - 1;

    // Check that the total length is under ESP_NOW_MAX_DATA_LEN
    if(packet_len > ESP_NOW_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Packet too big, can't transmit");
        return;
    }

    uint8_t packet_buffer[packet_len];
    example_espnow_data_t *header = (example_espnow_data_t *)packet_buffer;

    header->crc = 0;
    header->universe = universe;
    header->seq_num = sequence;
    header->data_length = data_length;

    memcpy(header->data, data, data_length);

    header->crc = crc16_le(UINT16_MAX, packet_buffer, packet_len);

    if (esp_now_send(s_example_broadcast_mac, packet_buffer, packet_len) != ESP_OK) {
        ESP_LOGE(TAG, "Send error");
    }
}

void app_main()
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    example_wifi_init();
    example_espnow_init();

    universe_stats_init();

#if defined(ROLE_SENDER)
    const uint32_t framedelay_ms = (1000/30);

    ESP_LOGI(TAG, "Starting sender mode...");

    // Red-to-blue fade
    uint8_t data[8*8*3];
//    uint8_t data[4*8*3];

    for(int led = 0; led<sizeof(data)/3; led++) {
        data[led*3+0] = led;
        data[led*3+1] = 0;
        data[led*3+2] = (sizeof(data)/3)-led;
    }

    uint8_t sequence = 0;
    while(true) {
        vTaskDelay(framedelay_ms/portTICK_RATE_MS);
//        ESP_LOGI(TAG, "Sending data seq:%i", sequence);

        for(int universe = 0; universe < 10; universe++)
            send_universe_data(universe, sequence, data, sizeof(data));

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
