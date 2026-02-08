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
        
        pynvml.nvmlShutdown()
        return {
            "gpu_load": gpu_load,
            "vram_used": round(vram_used, 1),
            "vram_total": round(vram_total, 1),
            "vram_p": round(vram_percent, 1),
            "gpu_temp": temp,
            "gpu_pwr": round(power_usage, 1)
        }
    except Exception as e:
        return {
            "gpu_load": 0, "vram_used": 0, "vram_total": 0, "vram_p": 0, "gpu_temp": 0, "gpu_pwr": 0
        }

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

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=False, help="Serial port (auto-detect if not specified)")
    parser.add_argument("--baud", type=int, default=115200)
    
    args = parser.parse_args()
    
    if not args.port:
        print("Auto-detecting ESP32...")
        args.port = auto_detect_esp32_port()
        if not args.port:
            print("ERROR: No ESP32 device found!", file=sys.stderr)
            print("\nAvailable ports:", file=sys.stderr)
            for port in serial.tools.list_ports.comports():
                print(f"  {port.device} - {port.description} (VID:PID {port.vid:04X}:{port.pid:04X})" if port.vid else f"  {port.device} - {port.description}", file=sys.stderr)
            sys.exit(1)
        print(f"Found ESP32 at: {args.port}")
    
    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
        print(f"Connected {args.port}")
    except Exception as e:
        print(f"ERROR: Cannot connect to {args.port}", file=sys.stderr)
        print(f"Reason: {e}", file=sys.stderr)
        print("\nTroubleshooting:", file=sys.stderr)
        print("1. Check if device is connected", file=sys.stderr)
        print("2. Check permissions: sudo usermod -a -G uucp $USER", file=sys.stderr)
        print("3. Try: ls -la /dev/ttyUSB*", file=sys.stderr)
        sys.exit(1)

    print("Running...")
    
    try:
        while True:
            cpu_percent = psutil.cpu_percent(interval=None)
            cpu_freq = psutil.cpu_freq().current if psutil.cpu_freq() else 0
            cpu_temp = get_cpu_temp()
            cpu_pwr = get_cpu_power()
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


            json_str = json.dumps(data)
            try:
                ser.write((json_str + '\n').encode('utf-8'))
            except Exception as e:
                pass
                
            time.sleep(1.0)
            
    except KeyboardInterrupt:
        ser.close()
    except Exception as e:
        ser.close()

if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print(f"CRITICAL ERROR: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(1)
