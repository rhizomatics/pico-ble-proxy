"""End-to-end tests for bleproxyd.

Runs the real binary against a simulated ESP32 (the serial protocol over
TCP) and checks it with Home Assistant's own ESPHome client library
(aioesphomeapi) and pyserial's RFC 2217 client (what esptool uses).

    python e2e.py path/to/bleproxyd [--long]

--long adds a 3-minute keepalive test.
"""

import argparse
import asyncio
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

import serial
from aioesphomeapi import APIClient

BINARY = None
FAILURES = []

ESP_BASE_MAC = "404CCA112230"
ESP_BT_MAC = "404CCA112232"
VICTRON = "00:11:22:33:44:01"
RUUVI = "00:11:22:33:44:02"
SWITCHBOT = "00:11:22:33:44:04"
STRANGER = "AA:BB:CC:DD:EE:FF"


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what)
    if not cond:
        FAILURES.append(what)


# --- simulated ESP32 ------------------------------------------------------


def crc16(b):
    c = 0xFFFF
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def cobs(src):
    out, idx, code = bytearray([0]), 0, 1
    for b in src:
        if b == 0:
            out[idx] = code
            idx = len(out)
            out.append(0)
            code = 1
        else:
            out.append(b)
            code += 1
            if code == 0xFF:
                out[idx] = code
                idx = len(out)
                out.append(0)
                code = 1
    out[idx] = code
    return bytes(out)


def frame(t, p):
    raw = bytes([t]) + p
    return b"\x00" + cobs(raw + struct.pack("<H", crc16(raw))) + b"\x00"


def adv(mac, atype, rssi, data, scan=0):
    return frame(2, bytes.fromhex(mac.replace(":", "")) + bytes([atype, rssi & 0xFF, scan]) + data)


HELLO = frame(1, bytes([1]) + bytes.fromhex(ESP_BASE_MAC) + bytes.fromhex(ESP_BT_MAC) + b"0.1.0-sim")


class FakeESP:
    """Serves the ESP side of the serial protocol on a TCP port."""

    def __init__(self):
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen()
        self.port = self.sock.getsockname()[1]
        self.conn = None
        self.hello_reqs = 0
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        while True:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            self.conn = conn
            threading.Thread(target=self._stream, args=(conn,), daemon=True).start()
            threading.Thread(target=self._read, args=(conn,), daemon=True).start()

    def _read(self, conn):
        while True:
            try:
                d = conn.recv(100)
            except OSError:
                return
            if not d:
                return
            if frame(0x81, b"") in d:
                self.hello_reqs += 1
                conn.sendall(HELLO)

    def _stream(self, conn):
        seq = 0
        try:
            conn.sendall(b"ESP-ROM:esp32c6-20220919\r\nboot noise\r\n" + HELLO)
            while True:
                seq += 1
                ruuvi = (
                    bytes.fromhex("020106") + bytes([0x1B, 0xFF, 0x99, 0x04, 0x05]) + bytes(20) + struct.pack(">H", seq)
                )
                victron = bytes.fromhex("020106") + bytes(
                    [0x0B, 0xFF, 0xE1, 0x02, 0x10, 0x00, seq & 0xFF, 0xA0, 1, 2, 3, 4]
                )
                conn.sendall(
                    adv(RUUVI, 1, -71, ruuvi)
                    + adv(RUUVI, 1, -71, b"", scan=1)  # empty scan response: must be dropped
                    + adv(STRANGER, 1, -90, bytes.fromhex("0201060303AAFE"))
                    + adv(VICTRON, 0, -60, victron)
                    + adv(SWITCHBOT, 0, -65, bytes.fromhex("0909576F53656E736F72"), scan=1)
                )
                time.sleep(0.1)
        except OSError:
            return

    def unplug(self):
        if self.conn:
            # shutdown() (not just close()) so the _read thread's blocking
            # recv() unblocks right away: on Linux, close() from another
            # thread doesn't interrupt a concurrent blocking call on the
            # same fd, so the socket wouldn't actually tear down (and send
            # its FIN) until the whole process exited.
            try:
                self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.conn.close()


# --- helpers ----------------------------------------------------------------


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Daemon:
    def __init__(self, args, conf):
        self.conf = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
        self.conf.write(conf)
        self.conf.close()
        self.log = tempfile.NamedTemporaryFile("w+", suffix=".log", delete=False)
        self.proc = subprocess.Popen([BINARY, *args, "-c", self.conf.name], stdout=self.log, stderr=self.log)

    def wait_log(self, text, timeout=5):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.output():
                return True
            time.sleep(0.05)
        return False

    def output(self):
        with open(self.log.name) as f:
            return f.read()

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        os.unlink(self.conf.name)


def mac_int(m):
    return int(m.replace(":", ""), 16)


# --- tests --------------------------------------------------------------------


async def test_home_assistant_flow(long_run):
    print("Home Assistant client flow")
    esp = FakeESP()
    api_port = free_port()
    d = Daemon(
        ["run"],
        f"""
serial tcp://127.0.0.1:{esp.port}
listen 127.0.0.1:{api_port}
allow_from 127.0.0.1
name test-proxy
friendly_name Test Proxy
flush_interval 200ms
min_interval 1s
keepalive 3s
device {VICTRON} Victron
device {RUUVI} Ruuvi
device {SWITCHBOT} SwitchBot
""",
    )
    try:
        check(d.wait_log("ESPHome API listening"), "API starts once the ESP says hello")
        cli = APIClient("127.0.0.1", api_port, password=None, client_info="e2e")
        await cli.connect(login=True)
        check((cli.api_version.major, cli.api_version.minor) == (1, 14), f"API version 1.14 (got {cli.api_version})")
        info = await cli.device_info()
        check(info.name == "test-proxy" and info.friendly_name == "Test Proxy", "device name")
        check(
            info.mac_address == ":".join(ESP_BASE_MAC[i : i + 2] for i in range(0, 12, 2)),
            "mac_address is the ESP base MAC",
        )
        check(info.bluetooth_mac_address.replace(":", "") == ESP_BT_MAC, "bluetooth_mac_address is the ESP BT MAC")
        flags = info.bluetooth_proxy_feature_flags_compat(cli.api_version)
        check(flags == 97, f"proxy flags passive+raw+state (got {flags})")
        ents, _ = await cli.list_entities_services()
        check(ents == [], "no entities")

        states = []
        cli.subscribe_bluetooth_scanner_state(lambda s: states.append((s.state, s.mode)))
        got = []
        t0 = time.monotonic()
        cli.subscribe_bluetooth_le_raw_advertisements(
            lambda m: got.extend(
                (time.monotonic() - t0, a.address, a.address_type, a.rssi, bytes(a.data)) for a in m.advertisements
            )
        )
        await asyncio.sleep(4.5)

        addrs = {g[1] for g in got}
        check(states and states[0] == (2, 1), f"scanner state running/active (got {states[:1]})")
        check({mac_int(VICTRON), mac_int(RUUVI), mac_int(SWITCHBOT)} <= addrs, "allowlisted devices forwarded")
        check(mac_int(STRANGER) not in addrs, "stranger filtered out")
        check(all(len(g[4]) > 0 for g in got), "no empty scan responses forwarded")
        ruuvi = [g for g in got if g[1] == mac_int(RUUVI)]
        # Snapshot + first forward at t=0, then one per min_interval (1 s): ~5-7 in 4.5 s, not the ~45 heard.
        check(4 <= len(ruuvi) <= 8, f"changing payload rate-limited to min_interval (got {len(ruuvi)} in 4.5 s)")
        check(all(g[3] == -71 and g[2] == 1 for g in ruuvi), "rssi and address type preserved")
        sb = [g for g in got if g[1] == mac_int(SWITCHBOT)]
        check(2 <= len(sb) <= 4, f"unchanged payload re-sent at keepalive (got {len(sb)} in 4.5 s, keepalive 3 s)")

        # Unplug the ESP: HA should hear the scanner stop, then start again.
        esp.unplug()
        await asyncio.sleep(1.0)
        check(any(s[0] == 5 for s in states), "scanner state stopped when ESP unplugged")
        await asyncio.sleep(3.0)
        check(states[-1][0] == 2, "scanner state running after ESP reconnects")
        check(esp.hello_reqs >= 2, "HELLO_REQ sent on each serial connect")

        if long_run:
            print("  (keepalive: waiting 170 s)")
            await asyncio.sleep(170)
            check(cli._connection is not None and cli._connection.is_connected, "client still connected after 170 s")
            check("timed out" not in d.output(), "no keepalive disconnect")

        await cli.disconnect()
        check(d.wait_log("disconnected"), "clean disconnect logged")
    finally:
        d.stop()


async def test_allow_from():
    print("allow_from")
    esp = FakeESP()
    api_port = free_port()
    d = Daemon(
        ["run"],
        f"""
serial tcp://127.0.0.1:{esp.port}
listen 127.0.0.1:{api_port}
allow_from 10.0.0.0/8
device {RUUVI} Ruuvi
""",
    )
    try:
        d.wait_log("ESPHome API listening")
        cli = APIClient("127.0.0.1", api_port, password=None)
        try:
            await asyncio.wait_for(cli.connect(login=True), 5)
            ok = False
        except Exception:
            ok = True
        check(ok, "connection from outside allow_from is refused")
        check("not in allow_from" in d.output(), "rejection logged")
    finally:
        d.stop()


def test_flash_bridge():
    print("flash-bridge (RFC 2217)")
    echo = socket.socket()
    echo.bind(("127.0.0.1", 0))
    echo.listen()
    echo_port = echo.getsockname()[1]

    def serve_echo():
        c, _ = echo.accept()
        while d := c.recv(4096):
            c.sendall(d)

    threading.Thread(target=serve_echo, daemon=True).start()
    port = free_port()
    d = Daemon(
        ["flash-bridge", "-t", "20"],
        f"""
serial tcp://127.0.0.1:{echo_port}
maintenance_listen 127.0.0.1:{port}
maintenance_allow_from 127.0.0.1
""",
    )
    try:
        check(d.wait_log("flash-bridge: listening"), "bridge listening")
        p = serial.serial_for_url(f"rfc2217://127.0.0.1:{port}", baudrate=460800, timeout=3)
        for dtr, rts in [(False, False), (True, False), (False, True), (False, False)]:  # usb-reset style
            p.dtr = dtr
            p.rts = rts
        payload = bytes(range(256)) * 64  # includes 0xFF (Telnet IAC)
        p.write(payload)
        got = b""
        while len(got) < len(payload):
            chunk = p.read(len(payload) - len(got))
            if not chunk:
                break
            got += chunk
        check(got == payload, f"16 KB binary round-trip ({len(got)} bytes back)")
        p.close()
        d.proc.wait(5)
        check(d.proc.returncode == 0, "bridge exits cleanly after the session")
    finally:
        d.stop()


def test_check_config():
    print("check-config")
    for conf, ok in [("device 00:11:22:33:44:01 x\n", True), ("colour blue\n", False)]:
        f = tempfile.NamedTemporaryFile("w", suffix=".conf", delete=False)
        f.write(conf)
        f.close()
        r = subprocess.run([BINARY, "check-config", "-c", f.name], capture_output=True, text=True)
        os.unlink(f.name)
        check((r.returncode == 0) == ok, f"{conf.strip()!r} -> {'accepted' if ok else 'rejected'}")


def main():
    global BINARY
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--long", action="store_true")
    a = ap.parse_args()
    BINARY = os.path.abspath(a.binary)
    test_check_config()
    asyncio.run(test_home_assistant_flow(a.long))
    asyncio.run(test_allow_from())
    test_flash_bridge()
    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILED")
        sys.exit(1)
    print("all e2e tests passed")


if __name__ == "__main__":
    main()
