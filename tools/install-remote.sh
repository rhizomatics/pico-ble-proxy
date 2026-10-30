#!/bin/sh
# Install or upgrade bleproxyd on an OpenWrt/RutOS host.
#
#   tools/install-remote.sh               # installs bleproxyd/dist/bleproxyd-linux-mipsle
#   REMOTE=root@192.168.1.1 tools/install-remote.sh
#   BIN=bleproxyd/dist/bleproxyd-linux-armv7 tools/install-remote.sh
#
# Uploads alongside the live files and checks the new binary against the host's
# config before stopping the service, so a bad upload leaves the old install
# running. An existing /etc/bleproxyd.conf is kept; the example is only copied
# when there is none. The installed files are listed in /etc/sysupgrade.conf so
# a firmware upgrade with "keep settings" preserves them.
#
# Uses one multiplexed SSH connection: The host OS may block clients that open several
# SSH connections in quick succession.
set -eu

REMOTE=${REMOTE:-root@192.168.1.1}
ROOT=$(dirname "$0")/../bleproxyd
BIN=${BIN:-$ROOT/dist/bleproxyd-linux-mipsle}
CP=${TMPDIR:-/tmp}/install-remote-$$.sock

[ -s "$BIN" ] || { echo "missing or empty $BIN; build it first, e.g. make -C bleproxyd mipsle" >&2; exit 1; }

echo "connecting to $REMOTE; ssh may ask for its password once, and the connection is reused after that"
ssh -fN -M -S "$CP" -o ControlPersist=10m "$REMOTE"
remote() { ssh -S "$CP" "$REMOTE" "$@" </dev/null; }
put() { scp -O -q -o "ControlPath=$CP" "$1" "$REMOTE:$2"; }

cleanup() {
	remote 'rm -f /usr/local/bin/bleproxyd.new /etc/init.d/bleproxyd.new' || true
	ssh -S "$CP" -O exit "$REMOTE" 2>/dev/null || true
}
trap cleanup EXIT

echo "uploading $BIN and the init script"
remote 'mkdir -p /usr/local/bin'
put "$BIN" /usr/local/bin/bleproxyd.new
put "$ROOT/packaging/procd/bleproxyd.init" /etc/init.d/bleproxyd.new

# Not a plain `if remote test`: a dropped connection must not read as "no config".
config=$(remote 'if [ -e /etc/bleproxyd.conf ]; then echo keep; else echo copy; fi')
if [ "$config" = copy ]; then
	echo "no /etc/bleproxyd.conf on $REMOTE; installing the example, edit it to suit"
	put "$ROOT/bleproxyd.conf.example" /etc/bleproxyd.conf
fi

echo "checking the new binary against /etc/bleproxyd.conf, then swapping it in and restarting bleproxyd"
# Expansions here are meant for the host's shell.
# shellcheck disable=SC2016
remote 'chmod +x /usr/local/bin/bleproxyd.new /etc/init.d/bleproxyd.new &&
	/usr/local/bin/bleproxyd.new check-config -c /etc/bleproxyd.conf || exit 1
	[ ! -x /etc/init.d/bleproxyd ] || /etc/init.d/bleproxyd stop
	killall bleproxyd 2>/dev/null; sleep 1
	mv /usr/local/bin/bleproxyd.new /usr/local/bin/bleproxyd &&
	mv /etc/init.d/bleproxyd.new /etc/init.d/bleproxyd &&
	/etc/init.d/bleproxyd enable && /etc/init.d/bleproxyd start || exit 1
	[ -z "$(tail -c1 /etc/sysupgrade.conf 2>/dev/null)" ] || echo >>/etc/sysupgrade.conf
	for f in /usr/local/bin/bleproxyd /etc/bleproxyd.conf /etc/init.d/bleproxyd \
		/etc/rc.d/S99bleproxyd /etc/rc.d/K10bleproxyd; do
		grep -qxF "$f" /etc/sysupgrade.conf 2>/dev/null || echo "$f" >>/etc/sysupgrade.conf
	done
	sysupgrade -l 2>/dev/null | grep bleproxyd ||
		echo "WARNING: sysupgrade -l does not list bleproxyd; it may not survive a firmware upgrade" >&2
	sleep 3; logread -e bleproxyd | tail'
