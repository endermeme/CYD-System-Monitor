#!/bin/bash
set -e

echo "Updating /opt/cyd-monitor..."

# Sync Python code
sudo cp -r monitor_host /opt/cyd-monitor/

# Sync runner script
sudo cp run.sh /opt/cyd-monitor/
sudo chmod +x /opt/cyd-monitor/run.sh

echo "✓ Updated code and runner in /opt/cyd-monitor"
echo "Test with: cyd-monitor"
