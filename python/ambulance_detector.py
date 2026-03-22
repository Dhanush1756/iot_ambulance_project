"""
Rakshak 2.0 — Ambulance Visual Detector
=========================================
Connects to ESP32-CAM stream, runs YOLOv8 on frames.
Calls on_detected / on_cleared callbacks when ambulance found/lost.

Supports:
  - Trained model (ambulance_yolov8s.onnx) — best accuracy
  - Default COCO model (yolov8n.pt) — fallback with light heuristic
"""

import cv2
import numpy as np
import threading
import time
import logging

log = logging.getLogger(__name__)
log.setLevel(logging.INFO)

# ── Config ───────────────────────────────────────────────────
YOLO_CONF_THRESH  = 0.45
CONFIRM_FRAMES    = 3      # consecutive detections to confirm
FLASH_CONF_THRESH = 0.30   # emergency light heuristic threshold

# HSV ranges for red/blue emergency lights
RED_HSV_LOWER1 = np.array([0,   120, 120])
RED_HSV_UPPER1 = np.array([10,  255, 255])
RED_HSV_LOWER2 = np.array([160, 120, 120])
RED_HSV_UPPER2 = np.array([180, 255, 255])
BLUE_HSV_LOWER = np.array([100, 100, 100])
BLUE_HSV_UPPER = np.array([130, 255, 255])
# ─────────────────────────────────────────────────────────────


def load_yolo(model_path):
    try:
        from ultralytics import YOLO
        model = YOLO(model_path)
        log.info(f"YOLO loaded: {model_path} | classes: {model.names}")
        return model
    except ImportError:
        log.warning("ultralytics not installed. Run: pip install ultralytics")
        return None
    except Exception as e:
        log.error(f"Failed to load model '{model_path}': {e}")
        log.warning("Falling back to emergency-light heuristic only")
        return None


def detect_lights(frame, roi=None):
    """Red/blue emergency light heuristic. Returns confidence 0–1."""
    region = frame[roi[1]:roi[3], roi[0]:roi[2]] if roi else frame
    if region.size == 0:
        return 0.0
    hsv   = cv2.cvtColor(region, cv2.COLOR_BGR2HSV)
    total = region.shape[0] * region.shape[1] + 1e-6
    r1 = cv2.inRange(hsv, RED_HSV_LOWER1, RED_HSV_UPPER1)
    r2 = cv2.inRange(hsv, RED_HSV_LOWER2, RED_HSV_UPPER2)
    red  = cv2.bitwise_or(r1, r2)
    blue = cv2.inRange(hsv, BLUE_HSV_LOWER, BLUE_HSV_UPPER)
    rr = np.sum(red  > 0) / total
    br = np.sum(blue > 0) / total
    return float(min(1.0, rr * 3.0 + br * 3.0 + rr * br * 10.0))


def label(frame, text, pos, color):
    cv2.putText(frame, text, pos, cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0,0,0), 3, cv2.LINE_AA)
    cv2.putText(frame, text, pos, cv2.FONT_HERSHEY_SIMPLEX, 0.7, color,   1, cv2.LINE_AA)


class AmbulanceDetector:
    def __init__(self, stream_url, model_path="ambulance_yolov8s.onnx",
                 on_detected=None, on_cleared=None, show_preview=True):
        self.stream_url   = stream_url
        self.model_path   = model_path
        self.on_detected  = on_detected
        self.on_cleared   = on_cleared
        self.show_preview = show_preview

        self._model    = None
        self._active   = False
        self._count    = 0
        self._lock     = threading.Lock()
        self._running  = False

    def is_ambulance_active(self):
        with self._lock:
            return self._active

    def start(self):
        self._model   = load_yolo(self.model_path)
        self._running = True
        t = threading.Thread(target=self._loop, daemon=True, name="CamDetector")
        t.start()
        log.info(f"Camera detector started → {self.stream_url}")

    def stop(self):
        self._running = False
        cv2.destroyAllWindows()

    def _loop(self):
        import urllib.request
        import numpy as np
        capture_url = self.stream_url.rstrip('/') + '/capture'
        log.info(f"Using capture endpoint: {capture_url}")

        while self._running:
            try:
                with urllib.request.urlopen(capture_url, timeout=3) as r:
                    jpg   = np.frombuffer(r.read(), dtype=np.uint8)
                    frame = cv2.imdecode(jpg, cv2.IMREAD_COLOR)
                    if frame is not None:
                        self._process(frame)
                    else:
                        time.sleep(0.05)
            except Exception as e:
                log.debug(f"Frame grab error: {e}")
                time.sleep(0.1)

    def _process(self, frame):
        detected = False
        conf     = 0.0
        boxes    = []

        if self._model is not None:
            results = self._model(frame, verbose=False, conf=YOLO_CONF_THRESH)
            for r in results:
                for box in r.boxes:
                    cid  = int(box.cls[0])
                    name = self._model.names[cid].lower()
                    bc   = float(box.conf[0])
                    x1, y1, x2, y2 = map(int, box.xyxy[0])

                    is_amb = 'ambulance' in name

                    # Fallback for COCO model: vehicle + light heuristic
                    if not is_amb and cid in [2, 5, 7]:
                        ls = detect_lights(frame, (x1, y1, x2, y2))
                        if ls > FLASH_CONF_THRESH:
                            is_amb = True
                            bc     = max(bc, ls)

                    if is_amb and bc >= YOLO_CONF_THRESH:
                        detected = True
                        conf     = max(conf, bc)
                        boxes.append((x1, y1, x2, y2, name, bc))
        else:
            # No model: full-frame light heuristic only
            ls = detect_lights(frame)
            if ls > FLASH_CONF_THRESH * 1.5:
                detected = True
                conf     = ls

        with self._lock:
            self._update(detected, conf)

        if self.show_preview:
            self._draw(frame, boxes, detected, conf)

    def _update(self, detected, conf):
        if detected:
            self._count = min(self._count + 1, CONFIRM_FRAMES + 2)
        else:
            self._count = max(self._count - 1, 0)

        if self._count >= CONFIRM_FRAMES and not self._active:
            self._active = True
            log.warning(f"🚑 AMBULANCE DETECTED (conf={conf:.2f})")
            if self.on_detected:
                threading.Thread(target=self.on_detected, daemon=True).start()

        elif self._count == 0 and self._active:
            self._active = False
            log.info("✅ Ambulance left frame")
            if self.on_cleared:
                threading.Thread(target=self.on_cleared, daemon=True).start()

    def _draw(self, frame, boxes, detected, conf):
        for (x1, y1, x2, y2, name, bc) in boxes:
            c = (0, 50, 255) if detected else (0, 200, 0)
            cv2.rectangle(frame, (x1, y1), (x2, y2), c, 2)
            label(frame, f"{name} {bc:.2f}", (x1, max(y1 - 8, 12)), c)

        txt   = "AMBULANCE DETECTED" if detected else "Monitoring..."
        color = (0, 50, 255) if detected else (100, 255, 100)
        label(frame, txt, (10, 35), color)

        cv2.imshow("Rakshak 2.0 — Camera", frame)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            self.stop()
