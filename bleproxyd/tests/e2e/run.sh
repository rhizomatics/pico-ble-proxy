#!/bin/sh
# Runs the e2e tests against build/bleproxyd. Uses $PYTHON if set, otherwise
# creates a virtualenv in tests/e2e/.venv with the pinned requirements.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
if [ -z "${PYTHON:-}" ]; then
	if [ ! -x "$here/.venv/bin/python" ]; then
		python3 -m venv "$here/.venv"
		"$here/.venv/bin/pip" install -q -r "$here/requirements.txt"
	fi
	PYTHON="$here/.venv/bin/python"
fi
exec "$PYTHON" -I "$here/e2e.py" "$here/../../build/bleproxyd" "$@"
