# POSEIDON C5 Node

Companion firmware for the ESP32-C5. The C5 is the only Espressif chip with:
- **5 GHz WiFi** (2.4 + 5, 802.11ax)
- **802.15.4** (Zigbee / Thread)

This firmware turns a C5 into a remote radio for POSEIDON (running on the Cardputer's S3). Communication happens over ESP-NOW — the C5 listens for command frames from POSEIDON, executes the requested scan or attack on its native radio, and streams results back.

## Protocol (over PigSync ESP-NOW)

```
struct posei_msg_t {
    uint32_t magic;       // 0x504F5345 "POSE"
    uint8_t  version;     // 2
    uint8_t  type;         // see POSEI_TYPE_*
    uint16_t seq;
    uint8_t  payload[244];
    uint8_t  payload_len;
} __packed;
```

Types:
| Type | Name | Direction | Payload |
|------|------|-----------|---------|
| 1    | HELLO            | both | node name, heap, gps |
| 10   | CMD_PING         | S3→C5 | — |
| 11   | CMD_SCAN_5G      | S3→C5 | duration_ms |
| 12   | CMD_SCAN_ZB      | S3→C5 | channel or 0xFF for hop |
| 13   | CMD_SCAN_2G      | S3→C5 | duration_ms |
| 14   | CMD_DEAUTH_5G    | S3→C5 | bssid + channel |
| 15   | CMD_STOP         | S3→C5 | — |
| 20   | RESP_PONG        | C5→S3 | — |
| 21   | RESP_AP_BATCH    | C5→S3 | count + array of APs |
| 22   | RESP_ZB_FRAME    | C5→S3 | pan id + src + dst |
| 23   | RESP_STATUS      | C5→S3 | last_cmd + ok flag |

## Building

Requires ESP-IDF v5.2 or newer (first version with C5 support).

```bash
. $IDF_PATH/export.sh
cd c5_node
idf.py set-target esp32c5
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Status

Skeleton. Implements HELLO + PING + basic 5 GHz scan. Zigbee sniffer is scaffolded but not wired in yet.
