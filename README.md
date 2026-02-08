# CYD System Monitor

ESP32-based system monitor với auto-detection và installation vào `/opt`.

## Features

- ✅ **Auto-detect ESP32**: Tự động nhận diện ESP32 qua USB vendor ID (Espressif, CH340, CP210x)
- ✅ **System-wide command**: Cài vào `/opt` và chạy từ bất kỳ đâu với `cyd-monitor`
- ✅ **Smart monitoring**: CPU, RAM, GPU, Network stats

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

## Troubleshooting

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
