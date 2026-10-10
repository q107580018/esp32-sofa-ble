#!/usr/bin/env bash
#
# Bounded, non-interactive serial capture for agents and scripts.
# See serial_log.py for the full option list.
#
#   ./tools/monitor.sh 10
#   ./tools/monitor.sh 10 --reset
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Deliberately NOT sourcing EIM's activate_idf_*.sh here:
#   - it is POSIX-sh code that reads $ZSH_VERSION, so it dies under `set -u`
#   - it prints ~20 lines of noise and defines aliases/completions we don't need
# All this script needs is a python that can `import serial`. EIM's ESP-IDF venv
# has pyserial; that is the whole dependency.
IDF_VENV="${IDF_VENV:-$HOME/.espressif/tools/python/v5.5.5/venv}"

if [[ -x "$IDF_VENV/bin/python" ]] && "$IDF_VENV/bin/python" -c 'import serial' 2>/dev/null; then
    PY="$IDF_VENV/bin/python"
elif command -v python3 >/dev/null && python3 -c 'import serial' 2>/dev/null; then
    PY="$(command -v python3)"
else
    echo "error: no python with pyserial found." >&2
    echo "hint: expected the ESP-IDF venv at $IDF_VENV -- check 'eim list'" >&2
    echo "hint: or run 'python3 -m pip install pyserial'" >&2
    exit 1
fi

exec "$PY" "$HERE/serial_log.py" "$@"
