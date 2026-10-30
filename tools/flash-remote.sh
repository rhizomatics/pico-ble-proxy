#!/bin/sh
# Reflash the ESP32 remotely through the host.
#
#   tools/flash-remote.sh                 # flashes firmware/build
#   REMOTE=root@192.168.1.1 tools/flash-remote.sh
#
# Stops bleproxyd, starts its RFC 2217 bridge, runs esptool against it, then
# restarts the service whatever happens. The ESP's ROM bootloader is reached
# through the USB-Serial-JTAG reset lines, so this works even if the running
# firmware is broken.
#
# Uses one multiplexed SSH connection: The host OS may block clients that open several
# SSH connections in quick succession.
set -eu

REMOTE=${REMOTE:-root@192.168.1.1}
HOST=${REMOTE#*@}
PORT=${PORT:-4000}
BUILD=${BUILD:-$(dirname "$0")/../firmware/build}
ESPTOOL=${ESPTOOL:-esptool}
CP=${TMPDIR:-/tmp}/flash-remote-$$.sock

[ -f "$BUILD/flash_args" ] || { echo "no $BUILD/flash_args; build the firmware first" >&2; exit 1; }

# flash_args is written at configure time, so it can outlive or predate the images it lists.
missing=0
while read -r addr image; do
	case $addr in
	0x*) [ -s "$BUILD/$image" ] || { echo "missing or empty $BUILD/$image" >&2; missing=1; } ;;
	esac
done <"$BUILD/flash_args"
[ "$missing" -eq 0 ] || { echo "build the firmware first" >&2; exit 1; }

echo "connecting to $REMOTE; ssh may ask for its password once, and the connection is reused after that"
ssh -fN -M -S "$CP" -o ControlPersist=10m "$REMOTE"
remote() { ssh -S "$CP" "$REMOTE" "$@" </dev/null; }

restore() {
	echo "restarting bleproxyd"
	remote 'killall bleproxyd 2>/dev/null; /etc/init.d/bleproxyd start' || true
	ssh -S "$CP" -O exit "$REMOTE" 2>/dev/null || true
}
trap restore EXIT

echo "stopping bleproxyd and starting its flash bridge"
remote '/etc/init.d/bleproxyd stop; sleep 1
	/usr/local/bin/bleproxyd flash-bridge -c /etc/bleproxyd.conf -t 120 </dev/null >/tmp/flash-bridge.log 2>&1 &'
sleep 2

echo "flashing the ESP32 through $HOST:$PORT"
cd "$BUILD"
"$ESPTOOL" --chip esp32c6 --port "rfc2217://$HOST:$PORT" \
	--before usb-reset --after hard-reset \
	write-flash @flash_args
