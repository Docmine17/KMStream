#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PI_HOST="${PI_HOST:-pi@10.42.0.92}"
PI_DIR="${PI_DIR:-~/KMStream}"

STATS_VAL=0
RUN_AFTER=0

for arg in "$@"; do
    case "${arg}" in
        --stats) STATS_VAL=1 ;;
        --run)   RUN_AFTER=1 ;;
        *)
            echo "Usage: $0 [--stats] [--run]" >&2
            exit 1
            ;;
    esac
done

echo "==> Syncing client sources to ${PI_HOST}:${PI_DIR}"
rsync -avz \
    "${REPO_ROOT}/common" \
    "${REPO_ROOT}/client" \
    "${REPO_ROOT}/scripts" \
    "${REPO_ROOT}/Makefile" \
    "${PI_HOST}:${PI_DIR}/"

echo "==> Building client on ${PI_HOST} (stats=${STATS_VAL})"
ssh "${PI_HOST}" "cd ${PI_DIR} && make client stats=${STATS_VAL}"

if [[ "${RUN_AFTER}" -eq 1 ]]; then
    echo "==> Running build/kmstream-pi on ${PI_HOST}"
    ssh -t "${PI_HOST}" "cd ${PI_DIR} && ./build/kmstream-pi"
fi
