# Build

## Prerequisites

| Tool | Needed for | Install |
|---|---|---|
| `make` | All `bleproxyd` builds | System: Xcode Command Line Tools on macOS, `build-essential` on Debian/Ubuntu |
| C compiler (`cc`, clang or gcc) with ASan/UBSan | `native`, `test`, `e2e` | System (as `make`) |
| `zig` | Cross-compiling (`mipsle`, `arm64`, `armv7`) | mise (in [mise.toml](mise.toml)) |
| `git` | Version string in the binary (falls back to `dev`) | System |
| Python 3 with `venv` | `e2e` (`aioesphomeapi`, `pyserial` are installed into `tests/e2e/.venv` automatically) | mise (`python`) or system |
| Docker | Building the ESP32 firmware with ESP-IDF | **Not via mise**: Docker Desktop, OrbStack, or the distro package |
| `uv` | Installing `esptool` | mise (`uv`) or the [uv installer](https://docs.astral.sh/uv/), or install packages using `pip` |
| `esptool` v5+ | Flashing the ESP32 (`tools/flash-remote.sh`) | `uv tool install esptool` |
| `ssh` | `tools/flash-remote.sh` | System |
| `pre-commit` | Git hooks (see [Pre-commit hooks](#pre-commit-hooks)) | mise (in [mise.toml](mise.toml)) |

> [!TIP]
> `mise` is the easiest way to install `zig`, Python and `uv`. Run `mise install` to follow the config [mise.toml](mise.toml) pins. Add the others with `mise use python uv` if you want them, then install `esptool` with `uv tool install esptool`.
>Docker, `ssh` and `git` can't be installed with mise. `make` and `clang` are only available through mise's experimental conda backend, so use the system versions.

### Getting `make` and a C compiler

| System | Command |
|---|---|
| macOS | `xcode-select --install` (Command Line Tools: `make`, Apple clang, `git`) |
| Debian, Ubuntu | `sudo apt install build-essential` (`make`, gcc), plus `clang` if you prefer it |
| Fedora, RHEL, Rocky, Alma | `sudo dnf install make gcc libasan libubsan`, plus `clang` if you prefer it |
| openSUSE | `sudo zypper install make gcc`, plus `clang` if you prefer it |
| Arch, Manjaro | `sudo pacman -S base-devel` (`make`, gcc), plus `clang` if you prefer it |

gcc and clang both work. The `test` target needs AddressSanitizer and UndefinedBehaviorSanitizer, which the Debian, Arch and macOS packages already include. On Fedora and RHEL they come from the separate `libasan` and `libubsan` packages. To build with clang on Linux, run `make CC=clang`.

## Linux

### Source

- `bleproxyd/`: C daemon for the router.
  - A static musl binary of about 170 KB that uses about 350 KB of RAM on the host device.
  - It's also built for `arm64`.
  - Implements just enough of the ESPHome API for a passive proxy, which is all that common BLE devices like Victron, Ruuvi and SwitchBot devices need.
- `common/linkcodec.c`: the serial framing, shared by the firmware and the daemon.

### Build

```sh
make -C bleproxyd test e2e mipsle     # unit + end-to-end tests, then bleproxyd/dist/bleproxyd-linux-mipsle
```

#### Make targets

| Target | Output | For |
|---|---|---|
| `mipsle` | `dist/bleproxyd-linux-mipsle` | MIPS32r2 little-endian, no FPU (Teltonika RUT95x and similar) |
| `arm64` | `dist/bleproxyd-linux-arm64` | aarch64 Linux (Pi 3/4/5 on a 64-bit OS, newer routers) |
| `armv7` | `dist/bleproxyd-linux-armv7` | 32-bit ARM with hardware float (older Pis, many OpenWrt ARM routers) |
| `native` | `build/bleproxyd` | The build machine, using the host `$(CC)` |
| `all` | all of the above | `native`, `mipsle`, `arm64` and `armv7` |
| `test` | `build/unit_test` | Unit tests, with ASan and UBSan |
| `e2e` | | End-to-end tests (see [Test](#test)) |
| `clean` | | Removes `build/` and `dist/` |

#### Other architectures

Builds are static musl via `zig cc`, so a new architecture only needs a new `-target` (and `-mcpu` where it matters). To add one, copy the `armv7` rule in [bleproxyd/Makefile](bleproxyd/Makefile), change the target, and add it to `.PHONY` and `all`.

Other likely targets:

- `x86_64-linux-musl`: PC or VM routers
- `mips-linux-musleabi`: big-endian MIPS, such as older Atheros/QCA devices
- `arm-linux-musleabi`: ARM without hardware float

To find what the host device needs, run `uname -m` on it. If it reports `mips`, also check the endianness with `cat /proc/cpuinfo` or `echo -n I | hexdump -o`.

## ESP32

### Source

- `firmware/`: ESP-IDF app.
  - Scans continuously and sends every advert over the framed serial link ([docs/serial-protocol.md](docs/serial-protocol.md)).
  - It has no Wi-Fi and no device-specific logic.
- `common/linkcodec.c`: the serial framing, shared by the firmware and the daemon.

### Build

```sh
docker run --rm -v "$PWD":/repo -w /repo/firmware espressif/idf:v5.5.1 idf.py build
```

## Test

The end-to-end tests ([bleproxyd/tests/e2e](bleproxyd/tests/e2e/README.md)) run the daemon against a simulated ESP and check it with Home Assistant's own ESPHome client library.

## Pre-commit hooks

[.pre-commit-config.yaml](.pre-commit-config.yaml) runs on every commit once installed:

```sh
pre-commit install --hook-type pre-commit --hook-type pre-push
pre-commit run --all-files            # check the whole tree
```

- Auto-fixes (re-stage and commit again): trailing whitespace, missing final newlines, CRLF line endings, Python import order and formatting (ruff, settings in [ruff.toml](ruff.toml)), C formatting (clang-format, settings in [.clang-format](.clang-format)).
- Checks: YAML/TOML/JSON syntax, merge conflict markers, large files and firmware images (`.bin`, `.uf2`, `.elf`), private keys, executable bits, shellcheck, ruff lint, codespell.
- When C sources or the Makefile change: `make test` and the `mipsle`, `arm64` and `armv7` cross builds.
- Before push only: `make e2e`, which is slower and builds a venv on first run.

## Install

- `tools/flash-remote.sh`: reflashes the ESP from home through the host, can be done remotely, e.g. if host+ESP32 are on a boat.
