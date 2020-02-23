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

#include "espnow_transponder.h"

#include "esp_wifi_internal.h"

static const char *TAG = "espnow";

// TODO
static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// ESPNOW can work in both station and softap mode. It is configured in menuconfig.
#if CONFIG_STATION_MODE
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#endif

#define ESPNOW_QUEUE_SIZE           30

#define IS_BROADCAST_ADDR(addr) (memcmp(addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

typedef enum {
    EXAMPLE_ESPNOW_SEND_CB,
    EXAMPLE_ESPNOW_RECV_CB,
} example_espnow_event_id_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} example_espnow_event_send_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int data_len;
} example_espnow_event_recv_cb_t;

typedef union {
    example_espnow_event_send_cb_t send_cb;
    example_espnow_event_recv_cb_t recv_cb;
} example_espnow_event_info_t;

// When ESPNOW sending or receiving callback function is called, post event to ESPNOW task.
typedef struct {
    example_espnow_event_id_t id;
    example_espnow_event_info_t info;
} example_espnow_event_t;



//! Packet format for espnow_transponder packets
typedef struct {
    uint16_t crc;
    uint8_t data_length;
    uint8_t data[0];
} __attribute__((packed)) espnow_transponder_packet_t;

static xQueueHandle espnow_transponder_queue;
static espnow_transponder_rx_callback_t rx_callback = NULL;

void espnow_transponder_register_callback(espnow_transponder_rx_callback_t callback) {
    rx_callback = callback;
}

void espnow_transponder_unregister_callback() {
    rx_callback = NULL;
}

esp_err_t espnow_transponder_send(const uint8_t *data, uint8_t data_length) {
    // Encapsulate the data into a packet with the following structure:
    // packet[0-1]: 16-bit CRC
    // packet[2]: data length
    // packet[3-n]: data

    const uint8_t packet_length = sizeof(espnow_transponder_packet_t) - 1 + data_length;

    // Check that the total length is under ESP_NOW_MAX_DATA_LEN
    if(packet_length > ESP_NOW_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Packet too big, can't transmit size:%i max:%i", packet_length, ESP_NOW_MAX_DATA_LEN);
        return ESP_FAIL;
    }

    uint8_t packet_data[packet_length];

    espnow_transponder_packet_t *header = (espnow_transponder_packet_t *)packet_data;
    header->crc = 0;
    header->data_length = data_length;
    memcpy(header->data, data, data_length);

    header->crc = crc16_le(UINT16_MAX, packet_data, packet_length);

    // TODO: Add length, CRC header
    return esp_now_send(s_example_broadcast_mac, packet_data, packet_length);
}

//! @brief Check if a buffer contains a valid espnow_transponder_packet_t
//!
//! \param data Pointer to the data packet
//! \param data_len Length of the data packet
//! \return True if the packet passed CRC + data length checks
bool parse_packet(uint8_t *packet, uint16_t packet_length)
{
    // Check the the packet can fit the header
    if (packet_length < sizeof(espnow_transponder_packet_t)) {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%i, minimum:%i", packet_length, sizeof(espnow_transponder_packet_t));
        return false;
    }

    espnow_transponder_packet_t *header = (espnow_transponder_packet_t *)packet;

    // Check the CRC
    const uint16_t crc = header->crc;
    header->crc = 0;    // Feed this back so we don't have to overwrite it
    const uint16_t crc_cal = crc16_le(UINT16_MAX, packet, packet_length);
    if(crc_cal != crc) {
        ESP_LOGE(TAG, "Failed CRC check, expected:%04x got:%04x", crc, crc_cal);
        return false;
    }

    const uint16_t expected_length = sizeof(espnow_transponder_packet_t) - 1 + header->data_length;
    if(expected_length != packet_length) {
        ESP_LOGE(TAG, "Invalid length, expected:%i got:%i", expected_length, packet_length);
        return false;
    }

    return true;
}

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
    if (xQueueSend(espnow_transponder_queue, &evt, portMAX_DELAY) != pdTRUE) {
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
    if (xQueueSend(espnow_transponder_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

//! @brief TX/RX callback handler task
static void example_espnow_task(void *pvParameter)
{
    example_espnow_event_t evt;

    while (xQueueReceive(espnow_transponder_queue, &evt, portMAX_DELAY) == pdTRUE) {
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
                    if(rx_callback != NULL) {
                        espnow_transponder_packet_t *packet = (espnow_transponder_packet_t *)recv_cb->data;
                        rx_callback(packet->data, packet->data_length);
                    }
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

//! @brief Initialize WiFi for use with ESP-NOW
static void wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK( esp_event_loop_init(example_event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());

    // In order to simplify example, channel is set after WiFi started.
    // This is not necessary in real application if the two devices have
    // been already on the same channel.
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, 0) );


    // From: https://www.esp32.com/viewtopic.php?t=9965
    // Attempt to change the wifi modulation speed
//    esp_wifi_internal_set_fix_rate(ESPNOW_WIFI_IF, 1, WIFI_PHY_RATE_MCS7_SGI);


#if CONFIG_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

//! @brief Initialize the ESP-NOW interface
static esp_err_t espnow_init(void)
{
    espnow_transponder_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (espnow_transponder_queue == NULL) {
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

esp_err_t espnow_transponder_init() {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    wifi_init();
    espnow_init();

    return ESP_OK; // TODO
}