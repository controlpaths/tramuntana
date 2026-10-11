#!/usr/bin/env bash
#
# Build a Tramuntana Lite r1a application (fw/apps/<app>) and produce the
# UF2 to flash onto the Pico 2. Uses a Zephyr workspace and SDK already
# installed on the host - no Docker, no west workspace of its own in this
# repo. Their locations come from fw/.env (copy .env.example and edit it);
# without one, ~/zephyrproject and ~/zephyr-sdk-1.0.1 are assumed.
#
# Usage:
#   ./build.sh [app] [board]
#   ./build.sh flash [app] [board]
#
#   app   - directory name under apps/ (default: tramuntana_fw)
#   board - west board target (default: rpi_pico2/rp2350a/m33)
#
# 'flash' also copies the UF2 onto the board. If apps/<app> exposes a USB
# CDC-ACM console (as tramuntana_fw does), the board doesn't need
# BOOTSEL held: sending the 'boot' shell command over that port makes the
# firmware reset itself straight into the ROM's mass storage bootloader.
# Otherwise, hold BOOTSEL yourself before running 'flash'.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Local paths (not versioned). Anything already set in the environment
# takes precedence over the file.
if [ -f "$SCRIPT_DIR/.env" ]; then
  while IFS='=' read -r key value; do
    value="${value%$'\r'}"
    [[ "$key" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
    if [ -z "${!key:-}" ]; then
      value="${value%\"}"; value="${value#\"}"
      eval "export $key=\"$value\""
    fi
  done < "$SCRIPT_DIR/.env"
fi

ZEPHYRPROJECT="${ZEPHYRPROJECT:-$HOME/zephyrproject}"
ZEPHYR_SDK="${ZEPHYR_SDK:-$HOME/zephyr-sdk-1.0.1}"
SDK_CMAKE="$ZEPHYR_SDK/cmake"
WEST="${WEST:-$ZEPHYRPROJECT/.venv/bin/west}"
ACM_PORT="${TRAMUNTANA_ACM_PORT:-/dev/ttyACM0}"

for d in "$ZEPHYRPROJECT/zephyr" "$SDK_CMAKE"; do
  if [ ! -d "$d" ]; then
    echo "Not found: $d - set ZEPHYRPROJECT / ZEPHYR_SDK in $SCRIPT_DIR/.env" >&2
    exit 1
  fi
done
if [ ! -x "$WEST" ]; then
  echo "west not found at $WEST - set WEST in $SCRIPT_DIR/.env" >&2
  exit 1
fi

ACTION="build"
if [ "${1:-}" = "flash" ]; then
  ACTION="flash"
  shift
fi

APP="${1:-tramuntana_fw}"
BOARD="${2:-rpi_pico2/rp2350a/m33}"

APP_DIR="$SCRIPT_DIR/apps/$APP"
BUILD_DIR="$SCRIPT_DIR/build/$APP"

if [ ! -d "$APP_DIR" ]; then
  echo "No such app: $APP_DIR" >&2
  exit 1
fi

export ZEPHYR_BASE="$ZEPHYRPROJECT/zephyr"
# The SDK's CMake package isn't registered under $HOME/.cmake/packages for
# this SDK copy, so point CMake at it directly instead.
export CMAKE_PREFIX_PATH="$SDK_CMAKE"

# west discovers the workspace (.west/, modules/, the SDK) by searching
# upward from the current directory, so run it from inside $ZEPHYRPROJECT
# and pass the app/build dirs as absolute paths.
cd "$ZEPHYRPROJECT"
"$WEST" build -p always -b "$BOARD" -s "$APP_DIR" -d "$BUILD_DIR"

UF2="$BUILD_DIR/zephyr/zephyr.uf2"
if [ ! -f "$UF2" ]; then
  echo "Build finished but no zephyr.uf2 was produced (check board/runner)." >&2
  exit 1
fi

echo ""
echo "UF2 ready: $UF2"

if [ "$ACTION" != "flash" ]; then
  echo "Flash: hold BOOTSEL while plugging in the Pico 2, then copy the UF2"
  echo "to the RP2350 mass storage device that appears (or run '$0 flash')."
  exit 0
fi

if [ -e "$ACM_PORT" ]; then
  echo "Sending 'boot' over $ACM_PORT to reset into the bootloader..."
  stty -F "$ACM_PORT" raw -echo 115200
  printf 'boot\r\n' > "$ACM_PORT"
else
  echo "No $ACM_PORT found - assuming the board is already in BOOTSEL mode"
  echo "(hold BOOTSEL and plug it in now if it isn't)."
fi

echo "Waiting for the bootloader's mass storage device..."
MOUNT=""
for _ in $(seq 1 20); do
  # grep finding nothing (still waiting) is expected every iteration but
  # one; `|| true` keeps that from tripping `set -e` and killing the loop.
  MOUNT="$(mount | grep -iE 'RP2350|RPI-RP2' | head -n1 | awk '{print $3}' || true)"
  [ -n "$MOUNT" ] && break
  sleep 0.5
done

if [ -z "$MOUNT" ]; then
  echo "Timed out waiting for the bootloader mass storage device." >&2
  echo "Hold BOOTSEL, plug the board in, and re-run: $0 flash $APP $BOARD" >&2
  exit 1
fi

echo "Copying UF2 to $MOUNT ..."
cp "$UF2" "$MOUNT/"
echo "Flashed. The board will reboot into the new firmware automatically."
