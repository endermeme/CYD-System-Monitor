# CYD System Monitor

ESP32-based system monitor với auto-detection và installation vào `/opt`.

## Features

- ✅ **Auto-detect ESP32**: Tự động nhận diện ESP32 qua USB vendor ID (Espressif, CH340, CP210x)
- ✅ **System-wide command**: Cài vào `/opt` và chạy từ bất kỳ đâu với `cyd-monitor`
- ✅ **Smart monitoring**: CPU, RAM, GPU, Network stats

## Dashboard

Một màn 320x240, cập nhật 1 Hz, sparkline ~150s cho CPU/GPU/NET/DISK:

| Khối | Số liệu |
|------|---------|
| Header | hostname, giờ, uptime, trạng thái link |
| CPU | load %, nhiệt package, xung trung bình, công suất package (RAPL, cần root), load average, số process, load từng thread (tách nhóm P/E trên CPU hybrid) |
| GPU (NVIDIA) | load %, nhiệt, quạt, PCIe gen/width hiện tại, xung core/mem, VRAM, công suất/giới hạn |
| MEM | RAM, swap, phân vùng `/` (used/total, %) |
| NET | RX/TX của interface default route |
| DISK | đọc/ghi tổng các ổ vật lý, nhiệt NVMe cao nhất |

Màu: cyan bình thường, vàng cảnh báo, đỏ nguy hiểm. Mất dữ liệu > 3s hiện banner `LINK LOST`.

Xem gói dữ liệu host gửi: `.venv/bin/python monitor_host/monitor.py --dump`

## Installation

```bash
# Cài đặt vào /opt (chỉ cần chạy 1 lần)
bash install_to_opt.sh

# Thêm user vào group uucp để access serial port
sudo usermod -a -G uucp $USER
# Sau đó log out và log in lại
```

## Usage

```bash
# Chạy monitor (từ bất kỳ đâu)
cyd-monitor

# Hoặc chạy trực tiếp với port cụ thể
cyd-monitor --port /dev/ttyUSB0
```

## Development

```bash
# Test local (không cần cài vào /opt)
bash run.sh

# Sau khi sửa code, update /opt
bash update_opt.sh
```

### Troubleshooting

### Nạp firmware mới
Firmware và host đổi format JSON cùng nhau, phải cập nhật cả hai:

```bash
# 1. Tắt service đang giữ cổng serial
sudo systemctl stop cyd-monitor

# 2. Upload firmware
cd monitor_firmware && ../.venv/bin/pio run -t upload && cd ..

# 3. Cập nhật host trong /opt và chạy lại
bash update_opt.sh
sudo systemctl start cyd-monitor
```

### Permission denied
```bash
sudo usermod -a -G uucp $USER
# Log out và log in lại
```

### No ESP32 found
```bash
# Kiểm tra devices
ls -la /dev/ttyUSB*

# Xem chi tiết USB devices
python3 -c "import serial.tools.list_ports; [print(f'{p.device} - {p.description} (VID:PID {p.vid:04X}:{p.pid:04X})') for p in serial.tools.list_ports.comports()]"
```
