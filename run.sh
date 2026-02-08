#!/bin/bash

source .venv/bin/activate

python3 monitor_host/monitor.py --port /dev/ttyUSB0
