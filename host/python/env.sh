# Source this file to get a `tramuntana` command in the current shell:
#
#   source host/python/env.sh
#   tramuntana i2c scan
#
# It only defines a shell function that calls host/python/.venv/bin/tramuntana
# - nothing is installed outside this folder and nothing is activated. The
# venv (with the package installed in editable mode) is created the first
# time if it doesn't exist. Lasts until you close the shell; `unset -f
# tramuntana` removes it earlier.

if [ -n "${BASH_SOURCE[0]:-}" ]; then
  _TRMT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
else
  _TRMT_DIR="$(cd "$(dirname "$0")" && pwd)"   # zsh
fi

if [ ! -x "$_TRMT_DIR/.venv/bin/tramuntana" ]; then
  echo "tramuntana: creating venv in $_TRMT_DIR/.venv ..."
  python3 -m venv "$_TRMT_DIR/.venv" &&
    "$_TRMT_DIR/.venv/bin/python" -m pip install --quiet -e "$_TRMT_DIR" ||
    echo "tramuntana: venv setup failed (Debian/Ubuntu: sudo apt install python3-venv)" >&2
fi

eval "tramuntana() { \"$_TRMT_DIR/.venv/bin/tramuntana\" \"\$@\"; }"
unset _TRMT_DIR
