"""
Webcam prototype of the eczema-vs-normal screen.

Two backends:
  clip   (default) CLIP ViT-B/16 with the fine-tuned RASH-vs-normal head
         (finetune_clip_eczema.py --target rash): held-out SCIN phone photos AUC 0.93, 82% of
         rashes caught, 93% of healthy skin cleared. Falls back to zero-shot prompts if the
         head file is missing. Stage 1 of the full app (app/server.py).
         Run with the main Python:  python3 scripts/webcam_eczema_demo.py
  esp32  the INT8 TFLite model the ESP32-CAM gets (train_esp32_rash_gate.py --data v1), with
         the device's exact preprocessing. 97% on its own test split, but that split's
         "normal" photos are stock images, and on a live webcam it mostly says NORMAL.
         Run with the TF venv:      .venv-tf/bin/python scripts/webcam_eczema_demo.py --backend esp32

    --image x.jpg [y.jpg ...]   classify photos instead of the webcam (no window)
    --camera 1                  use another camera

Hold the skin inside the white box, about 10-15 cm from the camera.

Keys
  q  quit
  e  save the current box as an ECZEMA / rash example -> experiments/webcam_captures/eczema/
  n  save the current box as a NORMAL-skin example -> experiments/webcam_captures/normal/

macOS asks for camera permission the first time: allow it for your terminal app, then
restart the terminal.
"""
import argparse
import time

import cv2
import numpy as np

from paths import ROOT

ESP32_MODEL = ROOT / "experiments" / "esp32_eczema_v1_mnv1_025" / "rash_gate_int8.tflite"
CAPTURE_DIR = ROOT / "experiments" / "webcam_captures"
SMOOTHING = 0.7  # weight on the previous frame's probability, to stop the label flickering
BOX_FRACTION = 0.6  # side of the on-screen box, as a fraction of the frame's shorter side

# Prompt set "eczema2" -- best of 4 sets compared on cached SCIN image embeddings
# (experiments/healthy_gate): AUC 0.900 eczema vs healthy.
PROMPTS = {
    "normal": ["a photo of healthy skin", "a photo of normal human skin",
               "a close-up photo of clear, even skin with no rash"],
    "eczema": ["a photo of atopic dermatitis", "a photo of an eczema rash",
               "a close-up photo of inflamed, red, flaky skin", "a photo of dermatitis on the skin"],
    "rash": ["a photo of a skin rash", "a close-up photo of a skin disease lesion",
             "a photo of inflamed, red, irritated skin", "a photo of a skin condition"],
    "no_skin": ["a photo of a room", "a photo of a person's face", "a photo of an object",
                "a blurry photo with no skin visible"],
}
# What a positive means for each backend: the CLIP head screens for any rash (stage 1 of
# the app), the ESP32 model was trained on eczema vs normal.
POSITIVE = {"clip": "RASH", "esp32": "ECZEMA"}
# Eczema-minus-normal logit margin that best balanced the two hit rates on SCIN.
CLIP_MARGIN = 3.35


class ClipModel:
    backend = "clip"

    def __init__(self):
        import torch
        from transformers import CLIPModel, CLIPProcessor
        self.torch = torch
        self.device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
        name = "openai/clip-vit-base-patch16"
        self.model = CLIPModel.from_pretrained(name).to(self.device).eval()
        self.proc = CLIPProcessor.from_pretrained(name)
        with torch.no_grad():
            text = []
            for key in ("normal", "rash", "no_skin"):
                t = self.model.get_text_features(**self.proc(
                    text=PROMPTS[key], return_tensors="pt", padding=True).to(self.device))
                t = (t / t.norm(dim=-1, keepdim=True)).mean(0)
                text.append(t / t.norm())
            self.text = torch.stack(text)
        # Fine-tuned head (finetune_clip_eczema.py) if present: a linear layer on the image
        # embedding, threshold folded into the bias. Otherwise the zero-shot prompts decide.
        head = ROOT / "experiments" / "clip_rash_head" / "head.npz"
        self.head = np.load(head) if head.exists() else None
        print("using the fine-tuned CLIP head" if self.head is not None else "using zero-shot CLIP")

    def predict(self, rgb_square):
        """Returns (P(eczema), skin_visible). P(eczema) is a sigmoid of the calibrated
        eczema-vs-normal score, so 0.5 sits exactly on the threshold."""
        with self.torch.no_grad():
            f = self.model.get_image_features(**self.proc(
                images=rgb_square, return_tensors="pt").to(self.device))
            f = f / f.norm(dim=-1, keepdim=True)
            logits = (100 * f @ self.text.T)[0].float().cpu().numpy()
            f = f[0].float().cpu().numpy()
        normal, rash, no_skin = logits
        score = f @ self.head["w"] + self.head["b"] if self.head is not None \
            else rash - normal - CLIP_MARGIN
        return float(1 / (1 + np.exp(-score))), bool(no_skin < max(normal, rash))


class Esp32Model:
    IMG = 96
    backend = "esp32"

    def __init__(self):
        import tensorflow as tf
        self.it = tf.lite.Interpreter(model_path=str(ESP32_MODEL))
        self.it.allocate_tensors()
        self.inp, self.out = self.it.get_input_details()[0], self.it.get_output_details()[0]
        self.in_scale, self.in_zero = self.inp["quantization"]
        self.out_scale, self.out_zero = self.out["quantization"]

    def predict(self, rgb_square):
        x = cv2.resize(rgb_square, (self.IMG, self.IMG), interpolation=cv2.INTER_AREA).astype(np.float32)
        q = np.clip(np.round(x / self.in_scale + self.in_zero), -128, 127).astype(np.int8)
        self.it.set_tensor(self.inp["index"], q[None])
        self.it.invoke()
        raw = self.it.get_tensor(self.out["index"]).astype(np.float32)[0, 0]
        return float((raw - self.out_zero) * self.out_scale), True


def center_box(frame, fraction):
    h, w = frame.shape[:2]
    side = int(min(h, w) * fraction)
    return (h - side) // 2, (w - side) // 2, side


def draw(frame, box, p, skin, fps, backend):
    y0, x0, side = box
    if not skin:
        colour, text = (200, 200, 200), "Point the camera at skin"
    elif p >= 0.5:
        colour, text = (60, 60, 230), f"{POSITIVE[backend]}  {p * 100:.0f}%"  # BGR
    else:
        colour, text = (80, 200, 80), f"NORMAL  {(1 - p) * 100:.0f}%"
    cv2.rectangle(frame, (x0, y0), (x0 + side, y0 + side), (255, 255, 255), 2)
    cv2.rectangle(frame, (0, 0), (frame.shape[1], 70), (30, 30, 30), -1)
    cv2.putText(frame, text, (20, 48), cv2.FONT_HERSHEY_SIMPLEX, 1.4, colour, 3, cv2.LINE_AA)
    if skin:
        bar_x, bar_w = frame.shape[1] - 320, 280
        cv2.rectangle(frame, (bar_x, 25), (bar_x + bar_w, 45), (90, 90, 90), -1)
        cv2.rectangle(frame, (bar_x, 25), (bar_x + int(bar_w * p), 45), (60, 60, 230), -1)
        cv2.putText(frame, "likelihood", (bar_x, 18), cv2.FONT_HERSHEY_SIMPLEX, 0.5,
                    (220, 220, 220), 1, cv2.LINE_AA)
    cv2.putText(frame, "Prototype screening aid - not a diagnosis",
                (20, frame.shape[0] - 40), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (200, 200, 200), 1,
                cv2.LINE_AA)
    cv2.putText(frame, f"{backend} | {fps:.0f} fps | e/n: save example | q: quit",
                (20, frame.shape[0] - 15), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 1,
                cv2.LINE_AA)


def save_example(crop_bgr, label):
    out = CAPTURE_DIR / label
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{label}_{time.strftime('%Y%m%d_%H%M%S')}_{int(time.time() * 1000) % 1000:03d}.jpg"
    cv2.imwrite(str(path), crop_bgr)
    print(f"saved {path.name} ({len(list(out.glob('*.jpg')))} {label} examples so far)")


def run_image(model, path):
    bgr = cv2.imread(str(path))
    if bgr is None:
        raise SystemExit(f"can't read {path}")
    y0, x0, side = center_box(bgr, 1.0)
    p, skin = model.predict(cv2.cvtColor(bgr[y0:y0 + side, x0:x0 + side], cv2.COLOR_BGR2RGB))
    verdict = "NO SKIN" if not skin else (POSITIVE[model.backend] if p >= 0.5 else "NORMAL")
    print(f"{path}: {POSITIVE[model.backend].lower()} likelihood {p:.3f} -> {verdict}")


def run_webcam(model, camera, backend):
    cap = cv2.VideoCapture(camera)
    if not cap.isOpened():
        raise SystemExit("can't open the webcam -- allow camera access for this terminal in "
                         "System Settings > Privacy & Security > Camera, then restart the terminal")
    p_smooth, t_prev, fps = 0.5, time.time(), 0.0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        frame = cv2.flip(frame, 1)  # mirror, so moving your arm feels natural
        box = center_box(frame, BOX_FRACTION)
        y0, x0, side = box
        crop = frame[y0:y0 + side, x0:x0 + side].copy()
        p, skin = model.predict(cv2.cvtColor(crop, cv2.COLOR_BGR2RGB))
        p_smooth = SMOOTHING * p_smooth + (1 - SMOOTHING) * p
        now = time.time()
        fps = 0.9 * fps + 0.1 / max(now - t_prev, 1e-6)
        t_prev = now
        draw(frame, box, p_smooth, skin, fps, backend)
        cv2.imshow("Eczema screen", frame)
        key = cv2.waitKey(1) & 0xFF
        if key == ord("q"):
            break
        if key == ord("e"):
            save_example(crop, "eczema")
        if key == ord("n"):
            save_example(crop, "normal")
    cap.release()
    cv2.destroyAllWindows()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backend", default="clip", choices=["clip", "esp32"])
    ap.add_argument("--image", nargs="+", help="classify these photos instead of the webcam")
    ap.add_argument("--camera", type=int, default=0, help="webcam index (try 1 for an external one)")
    args = ap.parse_args()
    model = ClipModel() if args.backend == "clip" else Esp32Model()
    if args.image:
        for path in args.image:
            run_image(model, path)
    else:
        run_webcam(model, args.camera, args.backend)


if __name__ == "__main__":
    main()
