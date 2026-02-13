import time
import json
import argparse
import psutil
import serial
import serial.tools.list_ports
import sys
import os
import warnings
warnings.filterwarnings("ignore", category=FutureWarning)

def auto_detect_esp32_port():
    esp32_vendors = [
        (0x10C4, 0xEA60),  # CP210x
        (0x1A86, 0x7523),  # CH340
        (0x303A, None),     # Espressif (any product)
    ]
    
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if port.vid is not None:
            for vendor_id, product_id in esp32_vendors:
                if port.vid == vendor_id:
                    if product_id is None or port.pid == product_id:
                        return port.device
    return None

try:
    import pynvml
except ImportError:
    pynvml = None

def get_nvidia_stats():
    if not pynvml:
        return {
            "gpu_load": 0, "vram_used": 0, "vram_total": 0, "vram_p": 0, "gpu_temp": 0, "gpu_pwr": 0
        }
    try:
        pynvml.nvmlInit()
        handle = pynvml.nvmlDeviceGetHandleByIndex(0)
        
        util = pynvml.nvmlDeviceGetUtilizationRates(handle)
        gpu_load = util.gpu
        
        mem = pynvml.nvmlDeviceGetMemoryInfo(handle)
        vram_used = mem.used / 1024**2
        vram_total = mem.total / 1024**2
        vram_percent = (vram_used / vram_total) * 100
        
        temp = pynvml.nvmlDeviceGetTemperature(handle, pynvml.NVML_TEMPERATURE_GPU)
        
        power_usage = pynvml.nvmlDeviceGetPowerUsage(handle) / 1000.0

        try:
            gpu_fan = pynvml.nvmlDeviceGetFanSpeed(handle)
        except:
            gpu_fan = 0

        pynvml.nvmlShutdown()
        return {
            "gpu_load": gpu_load,
            "vram_used": round(vram_used, 1),
            "vram_total": round(vram_total, 1),
            "vram_p": round(vram_percent, 1),
            "gpu_temp": temp,
            "gpu_pwr": round(power_usage, 1),
            "gpu_fan": gpu_fan
        }
    except Exception as e:
        return {
            "gpu_load": 0, "vram_used": 0, "vram_total": 0, "vram_p": 0, "gpu_temp": 0, "gpu_pwr": 0, "gpu_fan": 0
        }

def get_cpu_fan():
    try:
        fans = psutil.sensors_fans()
        if not fans:
            return 0
        for name, entries in fans.items():
            for entry in entries:
                if entry.current > 0:
                    return int(entry.current)
        return 0
    except:
        return 0

def get_cpu_temp():
    try:
        temps = psutil.sensors_temperatures()
        if not temps:
            return 0
        
        all_temps = []
        for name, entries in temps.items():
            if name in ['coretemp', 'k10temp', 'zenpower']:
                for entry in entries:
                    if 'Core' in entry.label or 'Tctl' in entry.label:
                        all_temps.append(entry.current)
        
        if all_temps:
            return sum(all_temps) / len(all_temps)
        
        return list(temps.values())[0][0].current
    except:
        return 0

last_rapl_energy = 0
last_rapl_time = 0

def get_cpu_power():
    global last_rapl_energy, last_rapl_time
    try:
        with open("/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj", "r") as f:
            energy = int(f.read())
            
        current_time = time.time()
        
        if last_rapl_time == 0:
            last_rapl_energy = energy
            last_rapl_time = current_time
            return 0.0
            
        delta_energy = energy - last_rapl_energy
        delta_time = current_time - last_rapl_time
        
        if delta_energy < 0:
            delta_energy = 0
            
        watts = (delta_energy / 1000000.0) / delta_time
        
        last_rapl_energy = energy
        last_rapl_time = current_time
        
        return round(watts, 1)
    except:
        return 0.0


class SerialManager:
    def __init__(self, port=None, baud=115200):
        self.port = port
        self.baud = baud
        self.serial = None
        self.connected = False
        self.backoff = 1
        self.max_backoff = 30
        
    def find_port(self):
        """Auto-detect ESP32 port if not manually specified"""
        if self.port:
            return self.port
            
        detected_port = auto_detect_esp32_port()
        if detected_port:
            print(f"Auto-detected ESP32 at {detected_port}")
            return detected_port
        return None

    def connect(self):
        """Attempt to connect to the serial port"""
        target_port = self.find_port()
        
        if not target_port:
            print("No ESP32 found. Retrying...", file=sys.stderr)
            return False

        try:
            self.serial = serial.Serial(target_port, self.baud, timeout=1)
            print(f"Connected to {target_port}")
            self.connected = True
            self.backoff = 1  # Reset backoff on successful connection
            self.port = target_port # Cache the found port 
            return True
        except Exception as e:
            print(f"Connection failed: {e}", file=sys.stderr)
            self.connected = False
            return False

    def disconnect(self):
        """Cleanly disconnect"""
        if self.serial:
            try:
                self.serial.close()
            except:
                pass
        self.serial = None
        self.connected = False
        print("Disconnected.")

    def write(self, data):
        """Write data to serial port, handle errors"""
        if not self.connected or not self.serial:
            return False
            
        try:
            json_str = json.dumps(data)
            self.serial.write((json_str + '\n').encode('utf-8'))
            return True
        except Exception as e:
            print(f"Write error: {e}", file=sys.stderr)
            self.disconnect()
            return False

    def run(self):
        """Main loop"""
        print("Starting Monitor with Auto-Reconnect...")
        
        while True:
            # Reconnection logic
            if not self.connected:
                if self.connect():
                    # Just connected, proceed immediately
                    pass 
                else:
                    # Connection failed, wait and retry
                    wait_time = self.backoff
                    print(f"Waiting {wait_time}s before retry...")
                    time.sleep(wait_time)
                    # Exponential backoff with jitter could be added, but simple doubling is fine
                    self.backoff = min(self.backoff * 2, self.max_backoff)
                    continue

            # Stats Collection
            try:
                cpu_percent = psutil.cpu_percent(interval=None)
                cpu_freq = psutil.cpu_freq().current if psutil.cpu_freq() else 0
                cpu_temp = get_cpu_temp()
                cpu_pwr = get_cpu_power()
                cpu_fan = get_cpu_fan()
                cpu_per_core = psutil.cpu_percent(interval=None, percpu=True)
                
                ram = psutil.virtual_memory()
                ram_used = ram.used / 1024**3
                ram_total = ram.total / 1024**3
                
                swap = psutil.swap_memory()
                swap_used = swap.used / 1024**3
                
                gpu_stats = get_nvidia_stats()
                
                disk = psutil.disk_usage('/')
                disk_used_p = disk.percent
                
                net = psutil.net_io_counters()
                net_sent = net.bytes_sent / 1024**2
                net_recv = net.bytes_recv / 1024**2
                
                data = {
                    "cpu": {
                        "load": round(cpu_percent, 1),
                        "temp": round(cpu_temp, 1),
                        "freq": round(cpu_freq, 0),
                        "pwr": cpu_pwr,
                        "fan": cpu_fan,
                        "cores": [round(c, 1) for c in cpu_per_core[:16]]
                    },
                    "ram": {
                        "used": round(ram_used, 1),
                        "total": round(ram_total, 1),
                        "p": round(ram.percent, 1)
                    },
                    "swap": {
                        "used": round(swap_used, 1),
                        "p": round(swap.percent, 1)
                    },
                    "gpu": gpu_stats,
                    "disk": {
                        "p": disk_used_p
                    },
                    "net": {
                        "sent": round(net_sent, 1),
                        "recv": round(net_recv, 1)
                    }
                }

                # Send Data
                if not self.write(data):
                    # If write failed, we are already disconnected by self.write()
                    # Loop will handle reconnection next iteration
                    pass

            except KeyboardInterrupt:
                print("Stopping...")
                break
            except Exception as e:
                print(f"Unexpected error in loop: {e}", file=sys.stderr)
                time.sleep(1) # Prevent tight loop on error
            
            time.sleep(1.0)
        
        self.disconnect()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=False, help="Serial port (auto-detect if not specified)")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    
    manager = SerialManager(port=args.port, baud=args.baud)
    manager.run()

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
    except Exception as e:
        print(f"CRITICAL ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
