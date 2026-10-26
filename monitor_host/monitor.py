import argparse
import json
import os
import re
import socket
import sys
import time
import warnings

import psutil
import serial
import serial.tools.list_ports

warnings.filterwarnings("ignore", category=FutureWarning)

try:
    import pynvml
except ImportError:
    pynvml = None

# Whole physical disks only; partitions, dm-* and zram would double count I/O.
PHYSICAL_DISK = re.compile(r"^(nvme\d+n\d+|sd[a-z]+|vd[a-z]+|mmcblk\d+)$")


def auto_detect_esp32_port():
    esp32_vendors = [
        (0x10C4, 0xEA60),  # CP210x
        (0x1A86, 0x7523),  # CH340
        (0x303A, None),     # Espressif (any product)
    ]

    for port in serial.tools.list_ports.comports():
        if port.vid is None:
            continue
        for vendor_id, product_id in esp32_vendors:
            if port.vid == vendor_id and (product_id is None or port.pid == product_id):
                return port.device
    return None


def read_file(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def cpu_model_name():
    """Short CPU name, e.g. '13th Gen Intel(R) Core(TM) i5-13400F' -> 'i5-13400F'."""
    for line in (read_file("/proc/cpuinfo") or "").splitlines():
        if line.startswith("model name"):
            name = line.split(":", 1)[1]
            name = re.sub(r"\(R\)|\(TM\)|@.*|\d+th Gen|\b(Intel|AMD|Core|CPU|Processor)\b"
                          r"|\d+-Core", "", name)
            return " ".join(name.split())[:20]
    return ""


def p_core_threads():
    """Number of P-core threads on Intel hybrid CPUs (0 if not hybrid)."""
    cpus = read_file("/sys/devices/cpu_core/cpus")
    if not cpus:
        return 0
    count = 0
    for part in cpus.split(","):
        lo, _, hi = part.partition("-")
        count += int(hi or lo) - int(lo) + 1
    return count


def default_interface():
    """Interface holding the default route, so docker/veth/VPN traffic is not summed."""
    for line in (read_file("/proc/net/route") or "").splitlines()[1:]:
        fields = line.split()
        if len(fields) > 1 and fields[1] == "00000000":
            return fields[0]
    return None


def cpu_temp(temps):
    for name in ("coretemp", "k10temp", "zenpower"):
        for entry in temps.get(name, []):
            if entry.label in ("Package id 0", "Tctl", "Tdie"):
                return entry.current
    for entries in temps.values():
        if entries:
            return entries[0].current
    return 0


def nvme_temp(temps):
    values = [e.current for e in temps.get("nvme", []) if e.label == "Composite"]
    return max(values) if values else 0


class Rate:
    """Turns a monotonically increasing counter into a per-second rate."""

    def __init__(self):
        self.last_value = None
        self.last_time = None

    def update(self, value):
        now = time.monotonic()
        rate = 0.0
        if self.last_value is not None and now > self.last_time and value >= self.last_value:
            rate = (value - self.last_value) / (now - self.last_time)
        self.last_value, self.last_time = value, now
        return rate


class Gpu:
    """NVML wrapper; initialised once, each metric read independently so one
    unsupported query does not blank the rest."""

    def __init__(self):
        self.handle = None
        if not pynvml:
            return
        try:
            pynvml.nvmlInit()
            self.handle = pynvml.nvmlDeviceGetHandleByIndex(0)
            name = pynvml.nvmlDeviceGetName(self.handle)
            if isinstance(name, bytes):
                name = name.decode()
            self.name = name.replace("NVIDIA ", "").replace("GeForce ", "")[:20]
        except Exception as e:
            print(f"NVML unavailable: {e}", file=sys.stderr)
            self.handle = None

    def _get(self, fn, *args, default=None):
        try:
            return fn(self.handle, *args)
        except Exception:
            return default

    def stats(self):
        if self.handle is None:
            return None
        n = pynvml
        util = self._get(n.nvmlDeviceGetUtilizationRates)
        mem = self._get(n.nvmlDeviceGetMemoryInfo)
        power = self._get(n.nvmlDeviceGetPowerUsage, default=0)
        limit = self._get(n.nvmlDeviceGetEnforcedPowerLimit, default=0)
        return {
            "n": self.name,
            "l": util.gpu if util else 0,
            "t": self._get(n.nvmlDeviceGetTemperature, n.NVML_TEMPERATURE_GPU, default=0),
            "w": round(power / 1000, 1),
            "wl": round(limit / 1000),
            "fan": self._get(n.nvmlDeviceGetFanSpeed),
            "cc": self._get(n.nvmlDeviceGetClockInfo, n.NVML_CLOCK_GRAPHICS, default=0),
            "mc": self._get(n.nvmlDeviceGetClockInfo, n.NVML_CLOCK_MEM, default=0),
            "pg": self._get(n.nvmlDeviceGetCurrPcieLinkGeneration, default=0),
            "pw": self._get(n.nvmlDeviceGetCurrPcieLinkWidth, default=0),
            "vu": round(mem.used / 1024**2) if mem else 0,
            "vt": round(mem.total / 1024**2) if mem else 0,
        }


class Collector:
    def __init__(self):
        self.host = socket.gethostname()
        self.cpu_name = cpu_model_name()
        self.p_threads = p_core_threads()
        self.gpu = Gpu()
        self.rapl = Rate()
        self.net_rx, self.net_tx = Rate(), Rate()
        self.disk_rd, self.disk_wr = Rate(), Rate()
        # Prime psutil's delta-based counters so the first sample is real.
        psutil.cpu_percent(percpu=True)
        psutil.cpu_percent()
        self.cpu_power()
        self.collect()

    def cpu_power(self):
        energy = read_file("/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj")
        if energy is None:  # root-only on most kernels
            return None
        return round(self.rapl.update(int(energy)) / 1e6, 1)

    def collect(self):
        temps = psutil.sensors_temperatures()
        freq = psutil.cpu_freq()
        ram = psutil.virtual_memory()
        swap = psutil.swap_memory()
        disk = psutil.disk_usage("/")

        io = psutil.disk_io_counters(perdisk=True) or {}
        rd = sum(v.read_bytes for k, v in io.items() if PHYSICAL_DISK.match(k))
        wr = sum(v.write_bytes for k, v in io.items() if PHYSICAL_DISK.match(k))

        iface = default_interface()
        nics = psutil.net_io_counters(pernic=True)
        net = nics.get(iface) if iface else None

        return {
            "h": self.host,
            "t": time.strftime("%H:%M:%S"),
            "up": int(time.time() - psutil.boot_time()),
            "cpu": {
                "n": self.cpu_name,
                "l": round(psutil.cpu_percent(), 1),
                "t": round(cpu_temp(temps)),
                "f": round(freq.current) if freq else 0,
                "w": self.cpu_power(),
                "c": [round(c) for c in psutil.cpu_percent(percpu=True)[:16]],
                "p": self.p_threads,
                "la": [round(x, 2) for x in os.getloadavg()],
                "pr": len(psutil.pids()),
            },
            "gpu": self.gpu.stats(),
            "ram": [round(ram.used / 1024**3, 1), round(ram.total / 1024**3, 1)],
            "swp": [round(swap.used / 1024**3, 1), round(swap.total / 1024**3, 1)],
            "dsk": {
                "u": round(disk.used / 1024**3),
                "t": round(disk.total / 1024**3),
                "r": round(self.disk_rd.update(rd)),
                "w": round(self.disk_wr.update(wr)),
                "tmp": round(nvme_temp(temps)),
            },
            "net": {
                "if": iface or "",
                "rx": round(self.net_rx.update(net.bytes_recv)) if net else 0,
                "tx": round(self.net_tx.update(net.bytes_sent)) if net else 0,
            },
        }


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

    def connect(self):
        target_port = self.find_port()
        if not target_port:
            print("No ESP32 found. Retrying...", file=sys.stderr)
            return False

        try:
            self.serial = serial.Serial(target_port, self.baud, timeout=1)
            print(f"Connected to {target_port}")
            self.connected = True
            self.backoff = 1
            self.port = target_port
            return True
        except Exception as e:
            print(f"Connection failed: {e}", file=sys.stderr)
            self.connected = False
            return False

    def disconnect(self):
        if self.serial:
            try:
                self.serial.close()
            except Exception:
                pass
        self.serial = None
        self.connected = False
        print("Disconnected.")

    def write(self, data):
        if not self.connected or not self.serial:
            return False

        try:
            line = json.dumps(data, separators=(",", ":")) + "\n"
            self.serial.write(line.encode("utf-8"))
            return True
        except Exception as e:
            print(f"Write error: {e}", file=sys.stderr)
            self.disconnect()
            return False

    def run(self):
        print("Starting Monitor with Auto-Reconnect...")
        collector = Collector()

        while True:
            if not self.connected:
                if not self.connect():
                    print(f"Waiting {self.backoff}s before retry...")
                    time.sleep(self.backoff)
                    self.backoff = min(self.backoff * 2, self.max_backoff)
                    continue

            started = time.monotonic()
            try:
                self.write(collector.collect())
            except KeyboardInterrupt:
                print("Stopping...")
                break
            except Exception as e:
                print(f"Unexpected error in loop: {e}", file=sys.stderr)

            # Keep a steady 1 Hz cadence regardless of collection time.
            time.sleep(max(0.0, 1.0 - (time.monotonic() - started)))

        self.disconnect()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=False, help="Serial port (auto-detect if not specified)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--dump", action="store_true", help="Print one sample as JSON and exit")
    args = parser.parse_args()

    if args.dump:
        collector = Collector()
        time.sleep(1)
        line = json.dumps(collector.collect(), separators=(",", ":"))
        print(line)
        print(f"{len(line)} bytes", file=sys.stderr)
        return

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
