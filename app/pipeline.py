"""
The three image models of the DermEdge prototype, chained, plus the flare-risk score.

  Stage 1  normal skin vs rash      CLIP ViT-B/16 + fine-tuned head (scripts/finetune_clip_eczema.py
                                    --target rash). Held-out SCIN phone photos: AUC 0.93. At one shared
                                    threshold it caught 84% of rashes on Fitzpatrick I-IV but 67% on V-VI
                                    (similar AUC, so a calibration gap); a self-reported skin type applies
                                    that group's threshold offset (fst_offsets.json), which brings V-VI to 83%.
  Stage 2  eczema vs 7 look-alikes  curated ResNet18 (models/curated_resnet18_balanced.pt), with a
                                    Grad-CAM heatmap. Curated test split: 81% accuracy, 82.5% recall.
                                    Trained on DermNet-style photos, so webcam accuracy is unmeasured.
  Stage 3  eczema severity          Ridge head on the same CLIP embedding (scripts/train_clip_severity.py),
                                    0-12 EASI-signs score -> Mild / Moderate / Severe.
  Flare risk = image_score x trigger_index (docs/PROJECT_HISTORY.md section 4): the trigger index is a
  noisy-OR of wearable markers (stress, skin hydration, night scratching) and journal triggers
  (stress, sleep, new products), so risk stays near zero
  without active eczema in the image.

Hackathon prototype: a screening aid, not a diagnosis.
"""
import json
import threading
from pathlib import Path

import numpy as np
import torch
from PIL import Image
from torchvision import models, transforms

ROOT = Path(__file__).resolve().parent.parent
EXP = ROOT / "experiments"
CLIP_NAME = "openai/clip-vit-base-patch16"
PROMPTS = {
    "normal": ["a photo of healthy skin", "a photo of normal human skin",
               "a close-up photo of clear, even skin with no rash"],
    "rash": ["a photo of a skin rash", "a close-up photo of a skin disease lesion",
             "a photo of inflamed, red, irritated skin", "a photo of a skin condition"],
    "no_skin": ["a photo of a room", "a photo of a person's face", "a photo of an object",
                "a blurry photo with no skin visible"],
}
LOOK_ALIKES = "psoriasis, tinea (ringworm), candidiasis, insect bites, lichen planus, drug eruption or rosacea"
UNSURE = (0.35, 0.65)  # stage-2 probabilities in this band are reported as uncertain


def fst_group(fitzpatrick):
    """Fitzpatrick type 1-6 (or None) -> the I-II / III-IV / V-VI groups the offsets use."""
    if not fitzpatrick:
        return None
    return "I-II" if fitzpatrick <= 2 else "III-IV" if fitzpatrick <= 4 else "V-VI"


def trigger_scores(wearable, journal):
    """Each trigger -> 0..1. `wearable` is wearable.Wearable.triggers() (stress, skin
    hydration, night scratching); the rest come from the journal."""
    s = dict(wearable)
    if journal.get("stress") is not None:
        s["stress (journal)"] = float(np.clip(journal["stress"] / 10, 0, 1)) * 0.8
    if journal.get("sleep_hours") is not None:
        s["poor sleep"] = float(np.clip((7 - journal["sleep_hours"]) / 4, 0, 1)) * 0.6
    if journal.get("new_product"):
        s["new soap / product"] = 0.4
    return s


def flare_risk(image_score, triggers):
    """Noisy-OR trigger index (1 - prod(1 - s_i)) times the image score."""
    index = 1 - float(np.prod([1 - v for v in triggers.values()])) if triggers else 0.0
    return dict(trigger_index=round(index, 3), risk=round(image_score * index, 3))


class SkinPipeline:
    def __init__(self):
        from transformers import CLIPModel, CLIPProcessor
        self.lock = threading.Lock()  # one model call at a time; requests run in a threadpool
        self.device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
        self.clip = CLIPModel.from_pretrained(CLIP_NAME).to(self.device).eval()
        self.proc = CLIPProcessor.from_pretrained(CLIP_NAME)
        with torch.no_grad():
            text = []
            for key in ("normal", "rash", "no_skin"):
                t = self.clip.get_text_features(**self.proc(
                    text=PROMPTS[key], return_tensors="pt", padding=True).to(self.device))
                t = (t / t.norm(dim=-1, keepdim=True)).mean(0)
                text.append(t / t.norm())
            self.text = torch.stack(text)
        self.rash_head = np.load(EXP / "clip_rash_head" / "head.npz")
        self.fst_offsets = json.loads((EXP / "clip_rash_head" / "fst_offsets.json").read_text())
        self.sev_head = np.load(EXP / "clip_severity_head" / "head.npz")

        # Stage 2 runs on CPU: Grad-CAM needs a backward pass, which is simplest there.
        self.resnet = models.resnet18(weights=None)
        self.resnet.fc = torch.nn.Linear(512, 2)  # index 1 = Eczema
        self.resnet.load_state_dict(torch.load(ROOT / "models" / "curated_resnet18_balanced.pt",
                                               map_location="cpu"))
        self.resnet.eval()
        self.resnet_tf = transforms.Compose([  # train_curated_cnn_balanced.py's eval transform
            transforms.Resize((224, 224)), transforms.ToTensor(),
            transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225])])

    @torch.no_grad()
    def _clip(self, img):
        f = self.clip.get_image_features(**self.proc(images=img, return_tensors="pt").to(self.device))
        f = f / f.norm(dim=-1, keepdim=True)
        logits = (100 * f @ self.text.T)[0].float().cpu().numpy()
        return f[0].float().cpu().numpy(), logits

    def _p_rash(self, f, fitzpatrick):
        logit = f @ self.rash_head["w"] + self.rash_head["b"]
        return float(1 / (1 + np.exp(-(logit + self.fst_offsets.get(fst_group(fitzpatrick), 0.0)))))

    def screen(self, img, fitzpatrick=None):
        """Stage 1 only, for the live feed."""
        with self.lock:
            f, (normal, rash, no_skin) = self._clip(img)
        return dict(p_rash=round(self._p_rash(f, fitzpatrick), 3), skin=bool(no_skin < max(normal, rash)))

    def _eczema_vs_lookalike(self, img):
        """Stage 2 + Grad-CAM on layer4, for the predicted class."""
        feats, grads = [], []
        h1 = self.resnet.layer4.register_forward_hook(lambda m, i, o: feats.append(o))
        h2 = self.resnet.layer4.register_full_backward_hook(lambda m, gi, go: grads.append(go[0]))
        try:
            x = self.resnet_tf(img)[None]
            out = self.resnet(x)
            p = float(torch.softmax(out.detach(), 1)[0, 1])
            direction = 1 if p >= 0.5 else -1
            self.resnet.zero_grad()
            (direction * (out[0, 1] - out[0, 0])).backward()
        finally:
            h1.remove()
            h2.remove()
        w = grads[0].mean((2, 3), keepdim=True)
        cam = torch.relu((w * feats[0]).sum(1))[0].detach().numpy()
        cam = cam / (cam.max() + 1e-8)
        return p, cam

    @staticmethod
    def overlay(img, cam):
        import matplotlib
        heat = Image.fromarray(np.uint8(cam * 255)).resize(img.size, Image.BILINEAR)
        rgb = matplotlib.colormaps["jet"](np.asarray(heat) / 255.0)[..., :3]
        a = np.asarray(img.convert("RGB"), dtype=np.float32) / 255
        alpha = 0.45 * (np.asarray(heat, dtype=np.float32) / 255)[..., None]
        return Image.fromarray(np.uint8(255 * ((1 - alpha) * a + alpha * rgb)))

    def analyze(self, img, wearable, journal, fitzpatrick=None):
        with self.lock:
            f, (normal, rash, no_skin) = self._clip(img)
            p_rash = self._p_rash(f, fitzpatrick)
            result = dict(p_rash=round(p_rash, 3), skin=bool(no_skin < max(normal, rash)),
                          p_eczema=None, severity=None, heatmap=None,
                          calibrated_for=fst_group(fitzpatrick))
            if not result["skin"]:
                result.update(verdict="no_skin", headline="No skin in view",
                              advice="Hold the skin inside the box, 10-15 cm from the camera.")
                return result, None
            if p_rash < 0.5:
                result.update(verdict="normal", headline="No rash detected",
                              advice="Skin looks normal. Keep logging triggers in your journal.")
                result["flare"] = flare_risk(0.0, trigger_scores(wearable, journal))
                result["triggers"] = trigger_scores(wearable, journal)
                return result, None
            p_ecz, cam = self._eczema_vs_lookalike(img)
        result["p_eczema"] = round(p_ecz, 3)
        unsure = UNSURE[0] <= p_ecz <= UNSURE[1]
        triggers = trigger_scores(wearable, journal)
        if p_ecz >= 0.5:
            score = float(np.clip(f @ self.sev_head["w"] + self.sev_head["b"], 0, 12))
            band = str(self.sev_head["classes"][int(np.digitize(score, self.sev_head["band_edges"]))])
            result["severity"] = dict(score=round(score, 1), band=band)
            result.update(verdict="eczema", headline=f"Likely eczema - {band.lower()}",
                          advice=("Moisturise, avoid your triggers, and see a doctor if it spreads, "
                                  "oozes or disturbs sleep." if band != "Severe" else
                                  "Looks severe - please book a doctor or dermatologist visit."))
            image_score = p_ecz * score / 12
        else:
            result.update(verdict="other_rash", headline="Rash - probably not eczema",
                          advice=f"It looks more like a look-alike condition ({LOOK_ALIKES}). "
                                 "These need different treatment - please see a doctor.")
            image_score = 0.0
        if unsure:
            result["headline"] += " (uncertain)"
            result["advice"] = "The models aren't sure. " + result["advice"] + \
                " A dermatologist should confirm."
        result["unsure"] = unsure
        result["triggers"] = triggers
        result["flare"] = flare_risk(image_score, triggers)
        return result, self.overlay(img, cam)
