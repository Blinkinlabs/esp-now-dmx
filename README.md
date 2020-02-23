# ESPNOW DMX transmission example

This prototype shows a method for sending DMX data using the ESP-NOW protocol.

ESP-NOW is a protocol that uses the WiFi modem to send packets, but avoids the overhead of retransmission and routing. This means that packets aren't guarinteed to reach their destination, but it seems ideal for a real-time, lossy friendly protocol like Art-Net

To run the demo, you'll need two ESP32 modules (any kind should be fine). Download and set up the ESP-IDF V3.3.1:

    https://docs.espressif.com/projects/esp-idf/en/v3.3.1/get-started/index.html

Then use it to flash this project onto one of the boards, with 'ROLE_SENDER' defined, and onto one or more boards with 'ROLE_SENDER' undefined.

When powered on, the board with 'ROLE_SENDER' defined will send a constant stream of packets at 30frames/s, and the receiver boards will receive the packets, and print some statistics every second.

Sample output:

    I (1193923) espnow_rx: universe: 0 packets:36681 oos:1406
    I (1193923) espnow_rx: universe: 1 packets:36883 oos:1270
    I (1193923) espnow_rx: universe: 2 packets:36611 oos:1521
    I (1193923) espnow_rx: universe: 3 packets:36293 oos:1717
    I (1193933) espnow_rx: universe: 4 packets:36024 oos:1977
    I (1193933) espnow_rx: universe: 5 packets:36120 oos:1880
    I (1193943) espnow_rx: universe: 6 packets:36359 oos:1734
    I (1193953) espnow_rx: universe: 7 packets:36316 oos:1729
    I (1193953) espnow_rx: universe: 8 packets:36192 oos:1810
    I (1193963) espnow_rx: universe: 9 packets:36279 oos:1805
    I (1193963) espnow_rx: universe:10 packets:0 oos:0
    I (1193973) espnow_rx: universe:11 packets:0 oos:0
    I (1193983) espnow_rx: universe:12 packets:0 oos:0
    I (1193983) espnow_rx: universe:13 packets:0 oos:0
    I (1193993) espnow_rx: universe:14 packets:0 oos:0
    I (1194993) espnow_rx: universe: 0 packets:36716 oos:1406
    I (1194993) espnow_rx: universe: 1 packets:36918 oos:1270
    I (1194993) espnow_rx: universe: 2 packets:36645 oos:1522
    I (1194993) espnow_rx: universe: 3 packets:36326 oos:1719
    I (1195003) espnow_rx: universe: 4 packets:36058 oos:1978
    I (1195003) espnow_rx: universe: 5 packets:36152 oos:1884
    I (1195013) espnow_rx: universe: 6 packets:36391 oos:1737
    I (1195023) espnow_rx: universe: 7 packets:36351 oos:1730
    I (1195023) espnow_rx: universe: 8 packets:36226 oos:1811
    I (1195033) espnow_rx: universe: 9 packets:36314 oos:1806
    I (1195033) espnow_rx: universe:10 packets:0 oos:0
    I (1195043) espnow_rx: universe:11 packets:0 oos:0
    I (1195053) espnow_rx: universe:12 packets:0 oos:0
    I (1195053) espnow_rx: universe:13 packets:0 oos:0
    I (1195063) espnow_rx: universe:14 packets:0 oos:0

Where 'packets' is the number of packets received by the universe, and 'oos' is the number of packets that were received out of sequence, indicating a dropped packet
