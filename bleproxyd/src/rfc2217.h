// Minimal RFC 2217 (Telnet COM-PORT-OPTION) server: enough for esptool's
// rfc2217:// port to reset and flash the ESP32 from another machine.
#pragma once

// Bridges one connected client to the serial port until the client
// disconnects. The serial port is reopened if the USB device re-enumerates
// during a reset. Returns 0 on a clean disconnect.
int rfc2217_session(int cfd, const char *serial_pattern);
