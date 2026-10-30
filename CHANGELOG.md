# Changelog

## 0.2.0 - 2026-10-30

### Added

- `tools/install-remote.sh` installs or upgrades `bleproxyd` on an OpenWrt/RutOS host, replacing the manual `scp` and `ssh` commands in the README. It uses the same `REMOTE` environment variable as `tools/flash-remote.sh`, and `BIN` selects a different build.
  - The new binary is checked against the host's config before the service is stopped, so a bad upload or config leaves the old install running.
  - An existing `/etc/bleproxyd.conf` is kept. The example config is only copied when there is none.
- `tools/flash-remote.sh` and `tools/install-remote.sh` print what they are doing at each step, including before `ssh` may ask for a password.

### Fixed

- `bleproxyd` can now be kept across RutOS firmware upgrades. Previously an upgrade could remove the whole install, including its config. `tools/install-remote.sh` lists the installed files in `/etc/sysupgrade.conf` and warns if `sysupgrade -l` doesn't report them. Existing installs need to run the script once to be registered, and firmware upgrades need "keep settings" on. This has not yet been confirmed across a real upgrade, so keep your own copy of `/etc/bleproxyd.conf`.
- `tools/flash-remote.sh` now checks that every image listed in `flash_args` exists and isn't empty before connecting. Previously a failed or interrupted build could get as far as stopping `bleproxyd` on the host before `esptool` failed.
