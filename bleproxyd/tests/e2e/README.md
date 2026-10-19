# End-to-end tests

`e2e.py` runs the real `bleproxyd` binary against a simulated ESP32 (the serial protocol served over
TCP) and checks it with the clients that matter:

- **aioesphomeapi**, Home Assistant's own ESPHome client. Checks the handshake, device identity,
  proxy feature flags, scanner state (including ESP unplug and replug), allowlist filtering,
  rate limiting and keepalive re-sends.
- **pyserial's RFC 2217 client**, which esptool uses. Checks Telnet negotiation, DTR/RTS control and
  a binary round-trip through `flash-bridge`.

```sh
make -C bleproxyd e2e                 # creates tests/e2e/.venv on first run
tests/e2e/run.sh --long               # adds a 3-minute keepalive test
PYTHON=/path/to/python tests/e2e/run.sh
```
