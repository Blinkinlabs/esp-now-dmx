#pragma once

//! \brief Initialize the ESP-NOW transponder
//!
//! \return ESP_OK if successful
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


//! \brief Get the maximum data size that can be transmitted with espnow_transponder
//!
//! \return Maximum data size, in bytes.
int espnow_transponder_max_packet_size();
