#!/usr/bin/env bash
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export ZEPHYR_SDK_INSTALL_DIR="$PROJECT_ROOT/toolchain"
export ZEPHYR_BASE="$PROJECT_ROOT/zephyr"
source "$PROJECT_ROOT/.venv-firmware/bin/activate"
export PATH="$PROJECT_ROOT/.venv-firmware/bin:$PATH"
echo "=== Zephyr Firmware Environment Activated ==="
echo "Board target: nucleo_f446re"
echo "SDK Toolchain: $ZEPHYR_SDK_INSTALL_DIR"
