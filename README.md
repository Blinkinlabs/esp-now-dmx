# ESPNOW DMX transmission example

This prototype shows a method for sending DMX data using the ESP-NOW protocol.

ESP-NOW is a protocol that uses the WiFi modem to send packets, but avoids the overhead of retransmission and routing. This means that packets aren't guarinteed to reach their destination, but it seems ideal for a real-time, lossy friendly protocol like Art-Net

To run the demo, you'll need two ESP32 modules (any kind should be fine). Download and set up the ESP-IDF V3.3.1:

    https://docs.espressif.com/projects/esp-idf/en/v3.3.1/get-started/index.html

Then use it to flash this project onto one of the boards, with 'ROLE_SENDER' defined, and onto one or more boards with 'ROLE_SENDER' undefined.

When powered on, the board with 'ROLE_SENDER' defined will send a constant stream of packets at 30frames/s, and the receiver boards will receive the packets, and print some statistics every second.
