#!/bin/bash
# Runs idf.py with ESP-IDF v5.4.1 activated, e.g. ./idf.sh build, ./idf.sh -p /dev/cu.SLAB_USBtoUART flash monitor
set -e
cd "$(dirname "$0")"
# The ESP-IDF activation script only works when sourced from a shell, so take its -e output.
while IFS='=' read -r key value; do
    case "$key" in
        PATH) export PATH="$value:$PATH" ;;
        SYSTEM_PATH) ;;
        [A-Z]*) export "$key=$value" ;;
    esac
done < <(bash "${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v5.4.1.sh}" -e)
idf.py "$@"
