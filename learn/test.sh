#!/usr/bin/env bash

set -e
set -o pipefail

if [ -z "$PYTHON_BIN_PATH" ]; then
    PYTHON_BIN_PATH=$(which python || which python3 || true)
fi

CONFIGURE_DIR=$(dirname "$0")

#"$PYTHON_BIN_PATH" "${CONFIGURE_DIR}/configure.py" "$@"

echo "${CONFIGURE_DIR}" "$@"
