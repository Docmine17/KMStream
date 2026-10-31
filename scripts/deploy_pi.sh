#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PI_HOST="${PI_HOST:-pi@10.42.0.92}"
PI_DIR="${PI_DIR:-~/KMStream}"

echo "==> Syncing client sources to ${PI_HOST}:${PI_DIR}"
rsync -avz \
    "${REPO_ROOT}/common" \
    "${REPO_ROOT}/client" \
    "${REPO_ROOT}/scripts" \
    "${REPO_ROOT}/Makefile" \
    "${PI_HOST}:${PI_DIR}/"

echo "==> Building client on ${PI_HOST}"
ssh "${PI_HOST}" "cd ${PI_DIR} && make client"

if [[ "${1:-}" == "--run" ]]; then
    echo "==> Running build/kmstream-pi on ${PI_HOST}"
    ssh -t "${PI_HOST}" "cd ${PI_DIR} && ./build/kmstream-pi"
fi
