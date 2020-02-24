#pragma once

// Note: This is private header from the esp-idf, that is included for wifi_phy_rate_t.
//       It might break across minor or major IDF versions. Consider wrapping wifi_phy_rate_t
//       if this bothers you.
#include <esp_wifi_internal.h>

//! ESP-NOW configuration settings
//!
//! There are some general rate categories to choose from:
//! ESP32 supports 802.11b/g/n, 
//! 'WIFI_PHY_RATE_xM_y are 802.11b settings: 1,2,5,5,11Mbps HR-DSS
//! 'WIFI_PHY_RATE_xM are 802.11g settings: 6,9,12,18,24,36,48,54Mbps OFDM,
//! 'WIFI_PHY_RATE_MCSx_yGI' are 802.11n(?) settings
//!
//! see:
//! * https://www.wlanpros.com/mcs-index-charts/
//! * https://www.intel.in/content/www/in/en/support/articles/000005725/network-and-i-o/wireless-networking.html
//!
typedef struct {
    wifi_mode_t mode;               //!< Either WIFI_MODE_STA or WIFI_MODE_AP
    int8_t power;                   //!< TX power, range is [40-82] -> [10dBm-20.5dBm]
    uint8_t channel;                //!< WiFi channel [1-13] (recommend: 1,6,11)
    wifi_phy_rate_t phy_rate;       //!< PHY rate (defined in esp_wifi_types.h)
} espnow_transponder_config_t;

//! Default transponder configuration
extern const espnow_transponder_config_t espnow_transponder_config_default;

//! \brief Initialize the ESP-NOW transponder
//!
//! \param config Configuration parameters, use espnow_transponder_config_default if unsure
//! \return ESP_OK if successful
esp_err_t espnow_transponder_init(const espnow_transponder_config_t *config);

//! \brief Broadcast a data packet
//!
//! \param data Pointer to the data packet
//! \param data_len Length of the data packet
//! \return ESP_OK if the packet was successfully queued
esp_err_t espnow_transponder_send(const uint8_t *data, uint8_t data_length);

//! Receive callback function prototype
//!
//! \param data Received packet data pointer
//! \param data_length Length of the data packet
typedef void (*espnow_transponder_rx_callback_t)(const uint8_t *data, uint8_t data_length);

//! \brief Register a callback for received data packets
//!
//! \param callback Callback function. Must have the same signature as espnow_transponder_callback_t
void espnow_transponder_register_callback(espnow_transponder_rx_callback_t callback);

//! \brief Unregister the received data callback function
void espnow_transponder_unregister_callback();


//! \brief Get the maximum data size that can be transmitted with espnow_transponder
//!
//! \return Maximum data size, in bytes.
int espnow_transponder_max_packet_size();
