#!/bin/bash

set -e

echo "Installing CYD Monitor to /opt/cyd-monitor..."

# Create opt directory
sudo mkdir -p /opt/cyd-monitor

# Copy files
sudo cp -r monitor_host /opt/cyd-monitor/
sudo cp -r monitor_firmware /opt/cyd-monitor/
sudo cp install.sh /opt/cyd-monitor/
sudo cp run.sh /opt/cyd-monitor/

# Create virtual environment
cd /opt/cyd-monitor
sudo python3 -m venv .venv
sudo .venv/bin/pip install --upgrade pip
sudo .venv/bin/pip install psutil pyserial pynvml

# Set permissions
sudo chown -R $USER:$USER /opt/cyd-monitor

# Create symlink in /usr/local/bin
sudo ln -sf /opt/cyd-monitor/run.sh /usr/local/bin/cyd-monitor

echo ""
echo "✓ Installation complete!"
echo "✓ You can now run: cyd-monitor"
echo ""
echo "Note: Make sure your user is in the 'uucp' group:"
echo "  sudo usermod -a -G uucp $USER"
echo "  Then log out and log back in."
