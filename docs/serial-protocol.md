# ESP32 ⇄ host serial protocol

The link runs over the ESP32-C6's built-in USB-Serial-JTAG port, which the host sees as `/dev/ttyACM*`.
The baud rate is ignored (USB CDC). Both directions use the same framing.

## Framing

```
0x00 COBS( type:u8 | payload:bytes | crc16:u16le ) 0x00
```

- **COBS** (Consistent Overhead Byte Stuffing) removes every `0x00` from the frame, so `0x00` only ever
  appears as the delimiter. A reader that starts mid-stream, or sees ROM boot chatter, drops bytes up
  to the next `0x00`. Senders put a delimiter on both sides of each frame, so noise can't merge into
  the frame that follows it. Empty frames between consecutive delimiters are ignored.
- **CRC** is CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`, no reflection, no xorout) over
  `type | payload`. Check value for `"123456789"` is `0x29B1`.
- Frames whose CRC doesn't match are dropped silently and counted.
- Maximum payload: 128 bytes.

## ESP → host

| Type | Name | Payload |
|---|---|---|
| `0x01` | HELLO | `proto:u8` (=1), `base_mac[6]`, `bt_mac[6]`, `firmware_version:utf8…` |
| `0x02` | ADV | `addr[6]` (MSB first, as printed), `addr_type:u8`, `rssi:i8`, `kind:u8` (0 = advertisement, 1 = scan response), `data[0..31]` (raw AD structures) |
| `0x03` | STATUS | `uptime_s:u32le`, `adv_seen:u32le`, `adv_dropped:u32le`, `free_heap:u32le` |
| `0x04` | LOG | `text:utf8…` |

`addr_type` uses NimBLE / ESPHome numbering: 0 public, 1 random, 2 public identity, 3 random identity.

The ESP sends HELLO at boot and whenever it receives HELLO_REQ. STATUS is sent every 30 s.

## Host → ESP

| Type | Name | Payload |
|---|---|---|
| `0x81` | HELLO_REQ | – |
| `0x82` | REBOOT | – |
