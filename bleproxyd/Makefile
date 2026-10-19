VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)

SRC := src/main.c src/config.c src/esphome.c src/hub.c src/rfc2217.c src/serial.c src/util.c ../common/linkcodec.c
HDR := $(wildcard src/*.h) ../common/linkcodec.h

CFLAGS ?= -Os
CFLAGS += -std=gnu11 -Wall -Wextra -Wshadow -Werror -I../common -DVERSION='"$(VERSION)"'
# Static, size-optimised Linux builds via zig's bundled musl.
CROSS_FLAGS := -static -s -ffunction-sections -fdata-sections -Wl,--gc-sections

.PHONY: all native test mipsle arm64 armv7 e2e clean

all: native mipsle arm64 armv7

native: build/bleproxyd

build/bleproxyd: $(SRC) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(SRC)

build/unit_test: tests/unit_test.c $(filter-out src/main.c,$(SRC)) $(HDR)
	@mkdir -p build
	$(CC) $(CFLAGS) -O0 -g -fsanitize=address,undefined -Isrc -o $@ tests/unit_test.c $(filter-out src/main.c,$(SRC))

test: build/unit_test
	./build/unit_test

# End-to-end tests against Home Assistant's own client library (needs Python
# with aioesphomeapi and pyserial: see tests/e2e/README.md).
e2e: native
	tests/e2e/run.sh

# MIPS routers such as the Teltonika 95X series: MIPS32r2 little-endian, no FPU.
mipsle: dist/bleproxyd-linux-mipsle

dist/bleproxyd-linux-mipsle: $(SRC) $(HDR)
	@mkdir -p dist
	zig cc -target mipsel-linux-musleabi -mcpu=mips32r2 $(CFLAGS) $(CROSS_FLAGS) -o $@ $(SRC)

# arm64 Linux (single-board computers etc).
arm64: dist/bleproxyd-linux-arm64

dist/bleproxyd-linux-arm64: $(SRC) $(HDR)
	@mkdir -p dist
	zig cc -target aarch64-linux-musl $(CFLAGS) $(CROSS_FLAGS) -o $@ $(SRC)

# 32-bit ARM with hardware float (older Pis, many OpenWrt ARM routers).
armv7: dist/bleproxyd-linux-armv7

dist/bleproxyd-linux-armv7: $(SRC) $(HDR)
	@mkdir -p dist
	zig cc -target arm-linux-musleabihf -mcpu=cortex_a7 $(CFLAGS) $(CROSS_FLAGS) -o $@ $(SRC)

clean:
	rm -rf build dist
