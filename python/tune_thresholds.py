"""
Rakshak 2.0 — ESP32 Threshold Calibration Tool
================================================
Reads ESP32 Serial Monitor output and calculates the perfect
ENERGY_THRESHOLD value for your specific mic and environment.

Usage:
  python tune_thresholds.py --list              (find your COM port)
  python tune_thresholds.py --port COM17        (Windows)
  python tune_thresholds.py --port /dev/ttyUSB0 (Linux/Mac)

Steps:
  Phase 1 - QUIET : Keep mic in silence for 30 sec → press Enter
  Phase 2 - SIREN : Play ambulance siren near mic  → press Enter
  Phase 3 - NOISE : Clap, talk, traffic sounds     → press Enter
  
  Script prints exact #define value to paste into traffic_unit.ino
"""

import serial
import serial.tools.list_ports
import re
import time
import argparse
import threading

try:
    import statistics
    HAS_STATS = True
except:
    HAS_STATS = False


# Matches the Serial output from traffic_unit.ino
# [FFT] rms=0.1936 ratio=0.02 peak=6 energy=✓ mod=✓
LINE_RE = re.compile(
    r'rms=([\d.]+).*ratio=([\d.]+).*peak=([\d.]+)'
)


def list_ports():
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found. Is ESP32 plugged in?")
        return
    print("\nAvailable serial ports:")
    for p in ports:
        print(f"  {p.device:15s}  {p.description}")
    print()


class PhaseData:
    def __init__(self, name):
        self.name   = name
        self.ratios = []
        self.rms    = []
        self.peaks  = []

    def add(self, rms, ratio, peak):
        self.ratios.append(float(ratio))
        self.rms.append(float(rms))
        self.peaks.append(float(peak))

    def summary(self):
        if not self.ratios:
            return f"{self.name}: no data collected"
        
        r_min  = min(self.ratios)
        r_max  = max(self.ratios)
        r_mean = sum(self.ratios) / len(self.ratios)
        p_max  = max(self.peaks)
        
        return (
            f"{self.name:6s} | "
            f"ratio  min={r_min:.4f}  max={r_max:.4f}  mean={r_mean:.4f} | "
            f"peak_max={p_max:.1f} | "
            f"samples={len(self.ratios)}"
        )


def run_tuner(port, baud=115200):
    print(f"\nConnecting to {port} at {baud} baud...")
    
    try:
        ser = serial.Serial(port, baud, timeout=2)
    except serial.SerialException as e:
        print(f"ERROR: Cannot open {port}")
        print(f"  {e}")
        print("Make sure ESP32 is connected and no other program is using the port.")
        return

    print(f"Connected to {port}!\n")
    print("=" * 60)
    print("  CALIBRATION GUIDE")
    print("=" * 60)
    print("  Phase 1 - QUIET : Complete silence near mic")
    print("  Phase 2 - SIREN : Play ambulance siren MP3 near mic")
    print("  Phase 3 - NOISE : Clap, talk, car sounds (false positive test)")
    print("  Press ENTER to advance to next phase")
    print("=" * 60)

    phases    = ["QUIET", "SIREN", "NOISE"]
    data      = {p: PhaseData(p) for p in phases}
    current   = [0]   # index into phases
    done      = [False]

    def input_thread():
        for i in range(len(phases)):
            input()  # wait for Enter
            current[0] += 1
            if current[0] < len(phases):
                print(f"\n>>> Phase {current[0]+1}: {phases[current[0]]} <<<")
                if phases[current[0]] == "SIREN":
                    print("    Play your ambulance siren MP3 LOUD near the mic now!")
                elif phases[current[0]] == "NOISE":
                    print("    Make noise: clap, talk, play music, car sounds...")
                print()
            else:
                done[0] = True

    t = threading.Thread(target=input_thread, daemon=True)
    t.start()

    print(f"\n>>> Phase 1: QUIET <<<")
    print("    Keep the environment SILENT. Press Enter when ready to advance.\n")

    line_count = 0
    try:
        while not done[0]:
            try:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
            except:
                break
                
            if not line:
                continue

            # Print every line so user can see it's working
            print(f"  {line}")

            # Parse FFT line
            m = LINE_RE.search(line)
            if m and current[0] < len(phases):
                rms, ratio, peak = m.groups()
                phase_name = phases[current[0]]
                data[phase_name].add(rms, ratio, peak)
                line_count += 1

    except KeyboardInterrupt:
        print("\n\nInterrupted by user.")
    finally:
        ser.close()

    if line_count == 0:
        print("\nERROR: No FFT data was received from ESP32.")
        print("Check that traffic_unit.ino is flashed and Serial Monitor baud = 115200")
        return

    # ── Print Results ─────────────────────────────────────────
    print("\n" + "=" * 60)
    print("  CALIBRATION RESULTS")
    print("=" * 60)
    for ph in phases:
        print(f"  {data[ph].summary()}")

    quiet_ratios = data["QUIET"].ratios
    siren_ratios = data["SIREN"].ratios
    noise_ratios = data["NOISE"].ratios

    print("\n" + "─" * 60)

    if not quiet_ratios or not siren_ratios:
        print("  Not enough data. Re-run and complete all 3 phases.")
        print("  Make sure to stay in each phase for at least 20 seconds.")
        return

    quiet_max = max(quiet_ratios)
    siren_min = min(siren_ratios)
    siren_max = max(siren_ratios)

    # Threshold = midpoint between quiet max and siren min
    recommended = (quiet_max + siren_min) / 2.0

    # Safety check
    if siren_min <= quiet_max:
        print(f"  ⚠️  WARNING: Siren min ({siren_min:.4f}) <= Quiet max ({quiet_max:.4f})")
        print("  The siren is not loud enough above background noise.")
        print("  Try: play siren LOUDER, or move phone CLOSER to mic.")
        recommended = quiet_max * 0.8  # fallback
    
    # Check noise vs threshold
    noise_ok = True
    if noise_ratios:
        noise_max = max(noise_ratios)
        if noise_max > recommended:
            print(f"  ⚠️  WARNING: Noise max ({noise_max:.4f}) > threshold ({recommended:.4f})")
            print("  Random noise may trigger false detections!")
            print("  Consider increasing threshold manually.")
            noise_ok = False

    print(f"\n  Quiet max ratio : {quiet_max:.4f}")
    print(f"  Siren min ratio : {siren_min:.4f}")
    print(f"  Siren max ratio : {siren_max:.4f}")
    if noise_ratios:
        print(f"  Noise max ratio : {max(noise_ratios):.4f}")

    print(f"\n  ✅ Recommended ENERGY_THRESHOLD = {recommended:.4f}")
    print(f"\n  ─────────────────────────────────────────────────────")
    print(f"  Paste this into traffic_unit.ino:")
    print(f"\n    #define ENERGY_THRESHOLD  {recommended:.4f}f")
    print(f"  ─────────────────────────────────────────────────────")

    # Also recommend MIN_PEAK_MAG
    siren_peak_min = min(data["SIREN"].peaks) if data["SIREN"].peaks else 1
    quiet_peak_max = max(data["QUIET"].peaks) if data["QUIET"].peaks else 0
    peak_thresh = max(1.0, (siren_peak_min + quiet_peak_max) / 2.0)

    print(f"\n  Also update MIN_PEAK_MAG:")
    print(f"\n    #define MIN_PEAK_MAG  {peak_thresh:.1f}f")
    print(f"  ─────────────────────────────────────────────────────")

    if noise_ok:
        print("\n  ✅ Noise test passed — threshold should avoid false positives")
    
    print("\n  After updating traffic_unit.ino, reflash the ESP32.")
    print("  Then run: python main.py --cam-url http://YOUR_IP/ --yolo-model ambulance_yolov8s.onnx\n")


def main():
    parser = argparse.ArgumentParser(
        description="Rakshak 2.0 — Threshold Calibration Tool"
    )
    parser.add_argument("--port", default=None,
                        help="Serial port e.g. COM17 or /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Baud rate (default 115200)")
    parser.add_argument("--list", action="store_true",
                        help="List available serial ports and exit")
    args = parser.parse_args()

    if args.list or args.port is None:
        list_ports()
        if args.port is None:
            print("Usage: python tune_thresholds.py --port COM17")
            print("       python tune_thresholds.py --list")
    else:
        run_tuner(args.port, args.baud)


if __name__ == "__main__":
    main()
