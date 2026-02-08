#!/bin/bash

# Setup
if [ ! -d ".venv" ]; then
    python3 -m venv .venv
fi

source .venv/bin/activate

pip install pyserial psutil pynvml platformio

cd monitor_firmware
pio run -t upload
cd ..

echo "Done."
