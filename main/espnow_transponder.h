#pragma once

//! \brief Initialize the wifi in esp-now mode
esp_err_t espnow_transponder_init();

//! \brief Broadcast a data packet
//!
//! \param data Pointer to the data packet
//! \param data_len Length of the data packet
//! \return ESP_OK if the packet was successfully queued
esp_err_t espnow_transponder_send(const uint8_t *data, uint8_t data_length);

typedef void (*espnow_transponder_rx_callback_t)(const uint8_t *data, uint8_t data_length);

//! \brief Register a callback for received data packets
//!
//! \param callback Callback function. Must have the same signature as espnow_transponder_callback_t
void espnow_transponder_register_callback(espnow_transponder_rx_callback_t callback);

//! \brief Unregister the received data callback function
void espnow_transponder_unregister_callback();


// Get the maximum packet size
// ESP_NOW_MAX_DATA_LEN - (sizeof(espnow_transponder_packet_t) - 1)
//int espnow_transponder_max_packet_size();
