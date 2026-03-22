"""
Rakshak 2.0 — Model Test (ESP32-CAM compatible)
Usage:
  python test_model.py --model ambulance_yolov8s.onnx --source http://192.168.92.99/
  python test_model.py --model ambulance_yolov8s.onnx --source webcam
"""

import argparse, time, cv2, os, urllib.request, numpy as np
from ultralytics import YOLO

def grab_frame(url):
    try:
        with urllib.request.urlopen(url, timeout=3) as r:
            jpg = np.frombuffer(r.read(), dtype=np.uint8)
            return cv2.imdecode(jpg, cv2.IMREAD_COLOR)
    except:
        return None

def run(model_path, source, conf=0.35):
    print(f"\nModel: {model_path}  Source: {source}  Conf: {conf}")
    print("Press Q to quit.\n")

    if not os.path.exists(model_path):
        print(f"ERROR: {model_path} not found. Copy .onnx file into python/ folder.")
        return

    model       = YOLO(model_path, task='detect')
    is_esp32    = source.startswith('http')
    capture_url = source.rstrip('/') + '/capture' if is_esp32 else None
    cap         = None

    if not is_esp32:
        cap = cv2.VideoCapture(0 if source == 'webcam' else source)
        if not cap.isOpened():
            print(f"ERROR: Cannot open {source}"); return

    frames = total_t = dets = 0
    print(f"Grabbing frames from: {capture_url or source}\n")

    while True:
        frame = grab_frame(capture_url) if is_esp32 else None
        if not is_esp32:
            ret, frame = cap.read()
            if not ret: break
        if frame is None:
            time.sleep(0.05); continue

        t0 = time.time()
        results = model.predict(frame, conf=conf, verbose=False)
        total_t += (time.time()-t0)*1000; frames += 1

        found = False
        for r in results:
            for box in r.boxes:
                name = model.names[int(box.cls[0])]
                bc   = float(box.conf[0])
                x1,y1,x2,y2 = map(int, box.xyxy[0])
                cv2.rectangle(frame,(x1,y1),(x2,y2),(0,50,255),2)
                lbl = f"{name} {bc:.2f}"
                (tw,th),_ = cv2.getTextSize(lbl,cv2.FONT_HERSHEY_SIMPLEX,0.7,2)
                cv2.rectangle(frame,(x1,y1-th-8),(x1+tw+4,y1),(0,50,255),-1)
                cv2.putText(frame,lbl,(x1+2,y1-5),cv2.FONT_HERSHEY_SIMPLEX,0.7,(255,255,255),2,cv2.LINE_AA)
                found=True; dets+=1
                print(f"  DETECTED: {name}  conf={bc:.2f}")

        status = "AMBULANCE DETECTED" if found else "Monitoring..."
        color  = (0,50,255) if found else (100,255,100)
        cv2.putText(frame,status,(10,38),cv2.FONT_HERSHEY_SIMPLEX,1.0,(0,0,0),3,cv2.LINE_AA)
        cv2.putText(frame,status,(10,38),cv2.FONT_HERSHEY_SIMPLEX,1.0,color,2,cv2.LINE_AA)
        fps = 1000/(total_t/frames) if frames else 0
        cv2.putText(frame,f"FPS:{fps:.0f} Det:{dets} Conf:{conf}",
                    (10,frame.shape[0]-10),cv2.FONT_HERSHEY_SIMPLEX,0.45,(200,200,200),1)
        cv2.imshow("Rakshak 2.0 - Test", frame)
        if cv2.waitKey(1) & 0xFF == ord('q'): break

    if cap: cap.release()
    cv2.destroyAllWindows()
    print(f"\nFrames:{frames}  Detections:{dets}  FPS:{1000/(total_t/frames if frames else 1):.1f}")

if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--model",  default="ambulance_yolov8s.onnx")
    p.add_argument("--source", default="webcam")
    p.add_argument("--conf",   type=float, default=0.35)
    a = p.parse_args()
    run(a.model, a.source, a.conf)