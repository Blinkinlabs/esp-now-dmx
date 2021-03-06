#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_now.h"
#include "rom/crc.h"

#include "espnow_transponder.h"

#include "esp_wifi_internal.h"

static const char *TAG = "espnow";

#define ESPNOW_ERROR_CHECK(function, message) \
    ret = (function); \
    if(ret!= ESP_OK) {  \
        ESP_LOGE(TAG, "Error running:%s, err:%s", message, esp_err_to_name(ret)); \
        return ret; \
    }

TaskHandle_t * espnow_transponder_task_hdl = NULL;

static espnow_transponder_stats_t espnow_transponder_stats;

//! Broadcast address
static const uint8_t broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

const espnow_transponder_config_t espnow_transponder_config_default = {
    .mode = WIFI_MODE_STA,
    .power = 90,
    .channel = 1,
    .phy_rate = WIFI_PHY_RATE_MCS2_LGI,
};

// Maximum size of the ESP-NOW event queue. Up to this many messages can be
// stored on reception. It's recommended to make the callback function process
// data fast enough that this queue can be small.
#define ESPNOW_QUEUE_SIZE           30

typedef enum {
    ESPNOW_TRANSPONDER_SEND_CB,
    ESPNOW_TRANSPONDER_RECV_CB,
    ESPNOW_TRANSPONDER_STOP_TASK,
} espnow_transponder_event_id_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
} espnow_transponder_event_send_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int data_len;
} espnow_transponder_event_recv_cb_t;

typedef union {
    espnow_transponder_event_send_cb_t send_cb;
    espnow_transponder_event_recv_cb_t recv_cb;
} espnow_transponder_event_info_t;

// When ESPNOW sending or receiving callback function is called, post event to ESPNOW task.
typedef struct {
    espnow_transponder_event_id_t id;       //!< Callback event type
    espnow_transponder_event_info_t info;   //!< Callback event data
} espnow_transponder_event_t;

//! Packet format for espnow_transponder packets
typedef struct {
    uint16_t crc;                       //!< 16-bit CRC, calculated with crc16_le()
    uint8_t data_length;                //!< Length of the data payload TODO: If ESP-NOW length is reliable, drop this
    uint8_t data[];                     //!< First element of the data payload
} __attribute__((packed)) espnow_transponder_packet_t;

//! Internal queue for handling espnow rx and tx callbacks
static xQueueHandle espnow_transponder_queue = NULL;

//! Pointer to the user function that is called when a packet is successfully received
static espnow_transponder_rx_callback_t rx_callback = NULL;

//! \brief Check if a buffer contains a valid espnow_transponder_packet_t
//!
//! \param data Pointer to the data packet
//! \param data_len Length of the data packet
//! \return True if the packet passed CRC + data length checks
static bool packet_check(uint8_t *packet, uint16_t packet_length)
{
    // Check the the packet can fit the header
    if (packet_length < sizeof(espnow_transponder_packet_t)) {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%i, minimum:%i", packet_length, sizeof(espnow_transponder_packet_t));
        espnow_transponder_stats.rx_short_packet++;
        return false;
    }

    espnow_transponder_packet_t *header = (espnow_transponder_packet_t *)packet;

    // Check the CRC
    const uint16_t crc = header->crc;
    header->crc = 0;    // TODO: Feed this into the CRC so we don't have to overwrite it
    const uint16_t crc_cal = crc16_le(UINT16_MAX, packet, packet_length);
    if(crc_cal != crc) {
        ESP_LOGE(TAG, "Failed CRC check, expected:%04x got:%04x", crc, crc_cal);

        espnow_transponder_stats.rx_bad_crc++;
        return false;
    }

    const uint16_t expected_length = sizeof(espnow_transponder_packet_t) + header->data_length;
    if(expected_length != packet_length) {
        ESP_LOGE(TAG, "Invalid length, expected:%i got:%i", expected_length, packet_length);

        espnow_transponder_stats.rx_bad_len++;
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

//! \brief ESP-NOW transmit callback function
//!
//! ESPNOW sending or receiving callback function is called in WiFi task.
//! Users should not do lengthy operations from this task. Instead, post
//! necessary data to a queue and handle it from a lower priority task.
//!
//! \param mac_addr MAC address that the packet was sent to
//! \param status transmit status
static void espnow_transponder_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    if (mac_addr == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    espnow_transponder_event_t evt = {
            .id = ESPNOW_TRANSPONDER_SEND_CB,
            .info.send_cb.status = status,
    };
    memcpy(evt.info.send_cb.mac_addr, mac_addr, sizeof(evt.info.send_cb.mac_addr));

    if (xQueueSend(espnow_transponder_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send to queue fail");
    }

    espnow_transponder_stats.tx_count++;
}

//! \brief ESP-NOW receive callback function
//!
//! ESPNOW sending or receiving callback function is called in WiFi task.
//! Users should not do lengthy operations from this task. Instead, post
//! necessary data to a queue and handle it from a lower priority task.
//!
//! \param mac_addr MAC address of the device that sent the packet
//! \param data Pointer to the packet data
//! \param len Length of the packet data
static void espnow_transponder_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len)
{
    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }
    
    if(!packet_check(data, len)) {
        ESP_LOGE(TAG, "Packet check failed");
        return;
    }

    espnow_transponder_event_t evt = {
        .id = ESPNOW_TRANSPONDER_RECV_CB,
        .info.recv_cb.data_len = len,
    };
    memcpy(evt.info.recv_cb.mac_addr, mac_addr, sizeof(evt.info.recv_cb.mac_addr));

    evt.info.recv_cb.data = malloc(len);
    if (evt.info.recv_cb.data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");

        espnow_transponder_stats.rx_malloc_fail++;
        return;
    }
    memcpy(evt.info.recv_cb.data, data, len);

    if (xQueueSend(espnow_transponder_queue, &evt, portMAX_DELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send to queue fail");
        free(evt.info.recv_cb.data);
    }

    espnow_transponder_stats.rx_count++;
}

//! \brief TX/RX callback handler task
static void espnow_transponder_task(void *pvParameter)
{
    espnow_transponder_event_t evt;

    while (xQueueReceive(espnow_transponder_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case ESPNOW_TRANSPONDER_SEND_CB:
            {
                espnow_transponder_event_send_cb_t *send_cb = &evt.info.send_cb;
                ESP_LOGD(TAG, "Sent data to "MACSTR", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);
                break;
            }
            case ESPNOW_TRANSPONDER_RECV_CB:
            {
                espnow_transponder_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                if(rx_callback != NULL) {
                    espnow_transponder_packet_t *packet = (espnow_transponder_packet_t *)recv_cb->data;
                    rx_callback(packet->data, packet->data_length);
                }

                free(recv_cb->data);
                break;
            }
//            case ESPNOW_TRANSPONDER_STOP_TASK:
//                goto stop_task;
//                break;
            default:
                ESP_LOGE(TAG, "Callback type error: %d", evt.id);
                break;
        }
    }

//stop_task:
    espnow_transponder_task_hdl = NULL;
    vTaskDelete(NULL);
}

//! \brief Initialize WiFi for use with ESP-NOW
static esp_err_t wifi_init(const espnow_transponder_config_t *config)
{
    tcpip_adapter_init();

    esp_err_t ret;

    // Don't fail if the event loop was already registered, it's not needed for this component.
    ret = esp_event_loop_init(example_event_handler, NULL);
    if(ret != ESP_OK) {
        ESP_LOGW(TAG, "Error running esp_event_loop_init, err:%s", esp_err_to_name(ret));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    // From: https://hackaday.io/project/161896-linux-espnow/log/161046-implementation
    // Disable AMPDU to allow the bit rate to be changed
    cfg.ampdu_tx_enable = 0;

    ESPNOW_ERROR_CHECK(esp_wifi_init(&cfg), "esp_wifi_init");
    ESPNOW_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM), "esp_wifi_set_storage");
    ESPNOW_ERROR_CHECK(esp_wifi_set_mode(config->mode), "esp_wifi_set_mode");

    ESPNOW_ERROR_CHECK(esp_wifi_start(), "esp_wifi_start");

    ESPNOW_ERROR_CHECK(esp_wifi_set_max_tx_power(config->power), "esp_wifi_set_max_tx_power");

    // In order to simplify example, channel is set after WiFi started.
    // This is not necessary in real application if the two devices have
    // been already on the same channel.
    // Note: With IDF 3.3.1, WiFi needs to be in promiscuous mode for the channel setting to work (?)
    ESPNOW_ERROR_CHECK(esp_wifi_set_promiscuous(true), "esp_wifi_set_promiscuous");
    ESPNOW_ERROR_CHECK(esp_wifi_set_channel(config->channel, WIFI_SECOND_CHAN_NONE), "esp_wifi_set_channel");

    // From: https://www.esp32.com/viewtopic.php?t=9965
    // Change the wifi modulation mode
    // See 'esp_wifi_types.h' for a list of available data rates
    const esp_interface_t interface = config->mode == WIFI_MODE_STA? ESP_IF_WIFI_STA : ESP_IF_WIFI_AP;
    ESPNOW_ERROR_CHECK(esp_wifi_internal_set_fix_rate(interface, true, config->phy_rate), "esp_wifi_internal_set_fix_rate");

    return ESP_OK;
}

//! \brief Initialize the ESP-NOW interface
static esp_err_t espnow_init(const espnow_transponder_config_t *config)
{
    espnow_transponder_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_transponder_event_t));
    if (espnow_transponder_queue == NULL) {
        ESP_LOGE(TAG, "Create queue fail");
        return ESP_FAIL;
    }

    esp_err_t ret;

    // Initialize ESPNOW and register sending and receiving callback function.
    ESPNOW_ERROR_CHECK(esp_now_init(), "esp_now_init");

    ESPNOW_ERROR_CHECK(esp_now_register_send_cb(espnow_transponder_send_cb), "esp_now_register_send_cb");
    ESPNOW_ERROR_CHECK(esp_now_register_recv_cb(espnow_transponder_recv_cb), "esp_now_register_recv_cb");

    const esp_interface_t interface = config->mode == WIFI_MODE_STA? ESP_IF_WIFI_STA : ESP_IF_WIFI_AP;

    // Add broadcast peer information to peer list.
    esp_now_peer_info_t peer = {
        .channel = config->channel,
        .ifidx = interface,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, broadcast_mac, sizeof(peer.peer_addr));

    ESPNOW_ERROR_CHECK(esp_now_add_peer(&peer), "esp_now_add_peer");

    // TODO: error check
    if(xTaskCreate(espnow_transponder_task, "espnow_task", 2048, NULL, 4, espnow_transponder_task_hdl) != pdPASS) {
        ESP_LOGE(TAG, "Create task fail");
        return ESP_FAIL;
    }

    return ESP_OK;
}

int espnow_transponder_max_packet_size() {
    return ESP_NOW_MAX_DATA_LEN - sizeof(espnow_transponder_packet_t);
}

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

    const uint8_t packet_length = sizeof(espnow_transponder_packet_t) + data_length;

    // Check that the total length is under ESP_NOW_MAX_DATA_LEN
    if(packet_length > ESP_NOW_MAX_DATA_LEN) {
        ESP_LOGE(TAG, "Packet too big, can't transmit size:%i max:%i", packet_length, ESP_NOW_MAX_DATA_LEN);
        return ESP_FAIL;
    }

    uint8_t packet[packet_length];

    espnow_transponder_packet_t *header = (espnow_transponder_packet_t *)packet;
    header->crc = 0;
    header->data_length = data_length;
    memcpy(header->data, data, data_length);

    header->crc = crc16_le(UINT16_MAX, packet, packet_length);

    ESP_LOGD(TAG, "header_len:%i data_lenth:%i packet_length%i", sizeof(espnow_transponder_packet_t),data_length, packet_length);

    // TODO: Add length, CRC header
    return esp_now_send(broadcast_mac, packet, packet_length);
}

esp_err_t espnow_transponder_init(const espnow_transponder_config_t *config) {
    if(config == NULL)
        return ESP_FAIL;

    // Initialize NVS 
    // Note: It's safe to call this multiple times.
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    if(ret != ESP_OK)
        return ret;

    ESPNOW_ERROR_CHECK(wifi_init(config), "wifi_init");
    ESPNOW_ERROR_CHECK(espnow_init(config), "espnow_init");

    return ESP_OK;
}

//! \brief Stop the ESP-NOW transponder
//!
//! Note: Need to test, correctly handle queue deletion
//esp_err_t espnow_transponder_stop() {
//
//    esp_now_deinit();
//    esp_now_unregister_recv_cb();
//    esp_now_unregister_send_cb();
//    esp_now_del_peer(broadcast_mac);
//
//    if(espnow_transponder_queue == NULL)
//        return ESP_FAIL;
//
//    espnow_transponder_event_t evt = {
//        .id = ESPNOW_TRANSPONDER_STOP_TASK,
//    };
//
//    if (xQueueSend(espnow_transponder_queue, &evt, portMAX_DELAY) != pdTRUE)
//        return ESP_FAIL;
//
//    return ESP_OK;
//}

void espnow_transponder_get_statistics(espnow_transponder_stats_t *stats) {

    // Note: There's no contention system here, so they might change during copy.
    memcpy(stats, &espnow_transponder_stats, sizeof(espnow_transponder_stats_t));
}
