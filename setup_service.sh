#!/bin/bash

if [ "$EUID" -ne 0 ]; then 
  echo "Please run as root"
  exit
fi

cp cyd-monitor.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable cyd-monitor.service
systemctl start cyd-monitor.service
systemctl status cyd-monitor.service
