#!/usr/bin/env bash
set -euo pipefail

PLUGIN_NAME="bilibili-live-obs"
BUILD_DIR="build"
OBS_PLUGIN_DIR="/usr/lib/obs-plugins"
OBS_DATA_DIR="/usr/share/obs/obs-plugins/${PLUGIN_NAME}"

cd "$(dirname "$0")"

echo "==> Building ${PLUGIN_NAME} ..."
cmake -B "${BUILD_DIR}" -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C "${BUILD_DIR}" -j"$(nproc)"

echo "==> Installing plugin to ${OBS_PLUGIN_DIR} ..."
sudo install -Dm755 "${BUILD_DIR}/lib${PLUGIN_NAME}.so" "${OBS_PLUGIN_DIR}/lib${PLUGIN_NAME}.so"

if [[ -d "data" && "$(ls -A data)" ]]; then
    echo "==> Installing data files to ${OBS_DATA_DIR} ..."
    sudo mkdir -p "${OBS_DATA_DIR}"
    sudo cp -r data/* "${OBS_DATA_DIR}/"
fi

echo "==> Done. Restart OBS Studio to load the plugin."
