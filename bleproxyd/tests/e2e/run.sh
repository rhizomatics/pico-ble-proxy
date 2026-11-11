#!/bin/sh
# Runs the e2e tests against build/bleproxyd. Uses $PYTHON if set, otherwise
# `uv run` with the pinned requirements, in an environment uv caches and rebuilds
# when the requirements or Python change.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
if [ -z "${PYTHON:-}" ]; then
	exec uv run --no-project --with-requirements "$here/requirements.txt" \
		python -I "$here/e2e.py" "$here/../../build/bleproxyd" "$@"
fi
exec "$PYTHON" -I "$here/e2e.py" "$here/../../build/bleproxyd" "$@"
