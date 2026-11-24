#!/usr/bin/env bash
# ==============================================================================
# Smart Grid Monitoring - Firmware Toolchain Uninstaller & Cleaner
# This script completely removes every file, directory, toolchain, and Homebrew
# package downloaded and installed specifically for compiling and flashing Zephyr.
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Removing local Zephyr workspace, modules, and virtual environments ==="
rm -rf .venv-firmware
rm -rf .west
rm -rf zephyr
rm -rf modules
rm -rf toolchain
rm -rf build
rm -rf .vscode

echo "=== Uninstalling Homebrew dependencies installed for firmware ==="
if command -v brew >/dev/null 2>&1; then
    brew uninstall dtc openocd wget || true
fi

echo "=== Cleanup complete! All downloaded firmware files have been deleted. ==="
