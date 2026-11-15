#!/usr/bin/env bash
# Installs the Python tooling (loop helper, tests, ESPHome for local compiles).
set -euo pipefail
cd "$(dirname "$0")/.."

# Docker creates the PlatformIO cache volume as root.
if [ -d "$HOME/.platformio" ] && [ ! -w "$HOME/.platformio" ]; then
  sudo chown "$(id -u):$(id -g)" "$HOME/.platformio"
fi

python3 -m venv .venv
.venv/bin/pip install --quiet --upgrade pip
.venv/bin/pip install --quiet -r requirements-dev.txt
.venv/bin/python -m unittest -q tests.test_jura_loop
