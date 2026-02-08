#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
cd "$SCRIPT_DIR"
"$SCRIPT_DIR/.venv/bin/python3" monitor_host/monitor.py
