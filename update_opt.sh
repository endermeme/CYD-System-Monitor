#!/bin/bash

# Quick update script - run this after making changes to run.sh
sudo cp /home/binhtagilla/Desktop/cyd-monitor/run.sh /opt/cyd-monitor/run.sh
sudo chmod +x /opt/cyd-monitor/run.sh

echo "✓ Updated /opt/cyd-monitor/run.sh"
echo "Test with: cyd-monitor"
