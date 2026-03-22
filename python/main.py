"""
Rakshak 2.0 — Main Python Script (Laptop)
==========================================
The brain of the system. Runs on your laptop.

What it does:
  1. Subscribes to MQTT — receives siren events from traffic ESP32
  2. Runs YOLOv8 on ESP32-CAM stream — detects ambulance visually
  3. AND logic: BOTH confirmed → publishes GREEN_AMBULANCE to traffic unit
  4. Also forwards ambulance approach alerts to car dashboard

MQTT flow:
  Traffic ESP32  → traffic/ambulance (siren_detected / siren_cleared)
  This script    → traffic/signal    (GREEN_AMBULANCE / RESET_NORMAL)
  This script    → traffic/camera    (ambulance_detected / ambulance_cleared)

Run:
  python main.py
  python main.py --cam-url http://192.168.1.105/
  python main.py --cam-url http://192.168.1.105/ --yolo-model ambulance_yolov8s.onnx
  python main.py --no-camera    (test siren MQTT only)
  python main.py --no-preview   (no OpenCV window)
"""

import argparse
import time
import logging
import signal
import sys
import threading

import paho.mqtt.client as mqtt

from ambulance_detector import AmbulanceDetector

logging.basicConfig(
    level   = logging.INFO,
    format  = '%(asctime)s  [%(name)-18s]  %(message)s',
    datefmt = '%H:%M:%S',
)
log = logging.getLogger("Rakshak2")

# ── MQTT Config ───────────────────────────────────────────────
BROKER_HOST  = "broker.hivemq.com"
BROKER_PORT  = 1883
CLIENT_ID    = "rakshak2_laptop"

# Topics (must match your ESP32 code)
TOPIC_SIREN   = "traffic/ambulance"   # ESP32 → laptop
TOPIC_CAMERA  = "traffic/camera"      # laptop → dashboards
TOPIC_SIGNAL  = "traffic/signal"      # laptop → traffic ESP32
TOPIC_STATUS  = "traffic/status"      # traffic ESP32 → dashboards
TOPIC_HB      = "traffic/heartbeat"
# ─────────────────────────────────────────────────────────────


class SystemState:
    def __init__(self):
        self.siren_active     = False
        self.ambulance_active = False
        self.signal_granted   = False
        self._lock            = threading.Lock()

    def evaluate(self, mqtt_client):
        """Call whenever either state changes. Applies AND logic."""
        with self._lock:
            both = self.siren_active and self.ambulance_active

            if both and not self.signal_granted:
                log.warning("━" * 52)
                log.warning("  🚨🚑  BOTH CONFIRMED — SENDING GREEN SIGNAL  🚑🚨")
                log.warning("━" * 52)
                mqtt_client.publish(TOPIC_SIGNAL, "GREEN_AMBULANCE", qos=1)
                self.signal_granted = True

            elif not both and self.signal_granted:
                log.info("Conditions cleared — resetting signal to normal")
                mqtt_client.publish(TOPIC_SIGNAL, "RESET_NORMAL", qos=1)
                self.signal_granted = False


def build_mqtt_client(state):
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=CLIENT_ID, clean_session=True)

    def on_connect(c, userdata, flags, rc):
        if rc == 0:
            log.info(f"MQTT connected → {BROKER_HOST}")
            c.subscribe(TOPIC_SIREN,  qos=1)
            c.subscribe(TOPIC_STATUS, qos=0)
            c.subscribe(TOPIC_HB,     qos=0)
            log.info(f"Subscribed: {TOPIC_SIREN}, {TOPIC_STATUS}")
        else:
            log.error(f"MQTT failed rc={rc}")

    def on_disconnect(c, userdata, rc):
        if rc != 0:
            log.warning("MQTT disconnected, auto-reconnecting...")

    def on_message(c, userdata, msg):
        topic   = msg.topic
        payload = msg.payload.decode('utf-8').strip()

        if topic == TOPIC_SIREN:
            if payload == "siren_detected" and not state.siren_active:
                log.warning("🔊 SIREN DETECTED (from traffic ESP32)")
                state.siren_active = True
                state.evaluate(c)

            elif payload == "siren_cleared" and state.siren_active:
                log.info("🔇 Siren cleared")
                state.siren_active = False
                state.evaluate(c)

        elif topic == TOPIC_HB:
            log.debug(f"Heartbeat: {payload}")

        elif topic == TOPIC_STATUS:
            log.debug(f"Traffic status: {payload}")

    client.on_connect    = on_connect
    client.on_disconnect = on_disconnect
    client.on_message    = on_message
    return client


def status_printer(state):
    while True:
        time.sleep(4)
        print(
            f"\r  [STATUS]  "
            f"Siren  = {'🔊 ACTIVE' if state.siren_active     else '🔇 none  '}"
            f"   Camera = {'🚑 ACTIVE' if state.ambulance_active else '⬜ none  '}"
            f"   Signal = {'🟢 GREEN (ambulance)' if state.signal_granted else '🔴 normal           '}   ",
            end='', flush=True
        )


def main():
    parser = argparse.ArgumentParser(description="Rakshak 2.0")
    parser.add_argument("--cam-url",    default="http://192.168.1.100/",
                        help="ESP32-CAM stream URL (e.g. http://192.168.1.105/)")
    parser.add_argument("--yolo-model", default="ambulance_yolov8s.onnx",
                        help="YOLO model (.onnx for CPU, .pt for GPU)")
    parser.add_argument("--no-camera",  action="store_true",
                        help="Disable camera — useful for testing audio only")
    parser.add_argument("--no-preview", action="store_true",
                        help="No OpenCV preview window (for headless machines)")
    args = parser.parse_args()

    log.info("━" * 52)
    log.info("  Rakshak 2.0 — Smart Emergency Traffic System")
    log.info("━" * 52)
    log.info(f"  Camera    : {'DISABLED' if args.no_camera else args.cam_url}")
    log.info(f"  YOLO model: {args.yolo_model}")
    log.info(f"  Broker    : {BROKER_HOST}")
    log.info("━" * 52 + "\n")

    state  = SystemState()
    client = build_mqtt_client(state)

    # Connect MQTT
    client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    client.loop_start()
    time.sleep(1.5)

    # Camera detector
    amb_det = None
    if not args.no_camera:
        def on_detected():
            if not state.ambulance_active:
                log.warning("🚑 CAMERA: Ambulance detected by YOLO")
                state.ambulance_active = True
                client.publish(TOPIC_CAMERA, "ambulance_detected", qos=1)
                state.evaluate(client)

        def on_cleared():
            if state.ambulance_active:
                log.info("✅ CAMERA: Ambulance left frame")
                state.ambulance_active = False
                client.publish(TOPIC_CAMERA, "ambulance_cleared", qos=1)
                state.evaluate(client)

        amb_det = AmbulanceDetector(
            stream_url   = args.cam_url,
            model_path   = args.yolo_model,
            on_detected  = on_detected,
            on_cleared   = on_cleared,
            show_preview = not args.no_preview,
        )
        amb_det.start()

    # Status thread
    threading.Thread(target=status_printer, args=(state,), daemon=True).start()

    log.info("System running. Waiting for events...")
    log.info("Press Ctrl+C to stop.\n")

    def shutdown(sig, frame):
        print()
        log.info("Shutting down gracefully...")
        if amb_det:
            amb_det.stop()
        client.publish(TOPIC_SIGNAL, "RESET_NORMAL", qos=1)
        time.sleep(0.5)
        client.loop_stop()
        client.disconnect()
        sys.exit(0)

    signal.signal(signal.SIGINT,  shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    while True:
        time.sleep(1)


if __name__ == "__main__":
    main()
