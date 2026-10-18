"""
Exports the DermEdge models for the browser version (web/), where everything runs on the
visitor's device:

  web/models/clip_text.json     CLIP ViT-B/16 text embeddings of the normal / rash / no-skin prompt
                                groups (the browser runs only the image encoder, via transformers.js)
  web/models/heads.json         stage-1 rash head + per-skin-type offsets, severity head, fairness table
  web/models/stage2.onnx        ResNet18 eczema vs look-alikes, with a second output (layer4 features)
                                so the page can draw a class activation map -- for a GAP + linear
                                head that is the same map Grad-CAM gives, without gradients
  web/models/stress_model.json  the calibrated WESAD LightGBM as a plain tree list for a JS evaluator

Then checks the ONNX model against PyTorch on the curated validation split.
"""
import ctypes
import json
import os

import numpy as np
import torch
import torch.nn as nn
from PIL import Image
from torchvision import models, transforms

ctypes.CDLL(os.path.join(os.path.dirname(torch.__file__), "lib", "libomp.dylib"), mode=ctypes.RTLD_GLOBAL)
import lightgbm as lgb  # noqa: E402
import pandas as pd  # noqa: E402

from paths import ROOT, MODELS_DIR, SKINDISEASE_DIR  # noqa: E402

OUT = ROOT / "web" / "models"
EXP = ROOT / "experiments"
PROMPTS = {  # app/pipeline.py
    "normal": ["a photo of healthy skin", "a photo of normal human skin",
               "a close-up photo of clear, even skin with no rash"],
    "rash": ["a photo of a skin rash", "a close-up photo of a skin disease lesion",
             "a photo of inflamed, red, irritated skin", "a photo of a skin condition"],
    "no_skin": ["a photo of a room", "a photo of an object", "a photo of an animal",
                "a photo of a landscape", "a photo with no human skin visible"],
    # Body parts count as skin: a live webcam frame of a hand or arm also shows the room around it.
    "body": ["a photo of a hand", "a photo of an arm", "a photo of a leg", "a photo of a foot",
             "a photo of a person's neck", "a close-up photo of human skin"],
}


def r(a, d=6):
    return [round(float(x), d) for x in np.asarray(a).ravel()]


@torch.no_grad()
def export_text():
    from transformers import CLIPModel, CLIPProcessor
    name = "openai/clip-vit-base-patch16"
    model, proc = CLIPModel.from_pretrained(name).eval(), CLIPProcessor.from_pretrained(name)
    out = {}
    for key, prompts in PROMPTS.items():
        t = model.get_text_features(**proc(text=prompts, return_tensors="pt", padding=True))
        t = (t / t.norm(dim=-1, keepdim=True)).mean(0)
        out[key] = r(t / t.norm())
    (OUT / "clip_text.json").write_text(json.dumps(dict(model=name, logit_scale=100, **out)))


def export_heads():
    rash = np.load(EXP / "clip_rash_head" / "head.npz")
    sev = np.load(EXP / "clip_severity_head" / "head.npz")
    summary = json.loads((EXP / "clip_rash_head" / "summary.json").read_text())
    heads = dict(
        rash=dict(w=r(rash["w"]), b=float(rash["b"]),
                  fst_offsets=json.loads((EXP / "clip_rash_head" / "fst_offsets.json").read_text())),
        severity=dict(w=r(sev["w"]), b=float(sev["b"]), band_edges=r(sev["band_edges"]),
                      classes=[str(c) for c in sev["classes"]]),
        fairness=dict(before=summary["by_fitzpatrick"], after=summary["by_fitzpatrick_calibrated"]),
    )
    (OUT / "heads.json").write_text(json.dumps(heads))


class Stage2(nn.Module):
    """ResNet18 returning (logits, layer4 feature map)."""

    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, x):
        n = self.net
        x = n.maxpool(n.relu(n.bn1(n.conv1(x))))
        f = n.layer4(n.layer3(n.layer2(n.layer1(x))))
        return n.fc(torch.flatten(n.avgpool(f), 1)), f


def export_stage2():
    net = models.resnet18(weights=None)
    net.fc = nn.Linear(512, 2)
    net.load_state_dict(torch.load(MODELS_DIR / "curated_resnet18_balanced.pt", map_location="cpu"))
    m = Stage2(net).eval()
    path = OUT / "stage2.onnx"
    torch.onnx.export(m, torch.zeros(1, 3, 224, 224), str(path), input_names=["image"],
                      output_names=["logits", "features"], opset_version=17, dynamo=False)
    # the fc weights the page needs for the activation map: eczema minus other
    w, b = net.fc.weight.detach(), net.fc.bias.detach()
    fc = dict(w=r(w[1] - w[0]), b=float(b[1] - b[0]))
    (OUT / "stage2_fc.json").write_text(json.dumps(fc))
    return m


def check_stage2(m):
    import onnxruntime as ort
    sess = ort.InferenceSession(str(OUT / "stage2.onnx"))
    tf = transforms.Compose([transforms.Resize((224, 224)), transforms.ToTensor(),
                             transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225])])
    val = pd.read_csv(SKINDISEASE_DIR / "manifest_curated_v3_val.csv").sample(120, random_state=0)
    pt, ox = [], []
    for p in val["path"]:
        x = tf(Image.open(ROOT / p).convert("RGB"))[None]
        with torch.no_grad():
            pt.append(torch.softmax(m(x)[0], 1)[0, 1].item())
        lo = sess.run(["logits"], {"image": x.numpy()})[0][0]
        ox.append(float(np.exp(lo[1]) / np.exp(lo).sum()))
    pt, ox = np.array(pt), np.array(ox)
    print(f"stage2 ONNX vs PyTorch on {len(pt)} val images: max |dp| {np.abs(pt - ox).max():.2e}, "
          f"same decision {np.mean((pt >= .5) == (ox >= .5)):.3f}")


def export_stress():
    b = lgb.Booster(model_file=str(MODELS_DIR / "wesad_stress_lightgbm.txt"))
    d = b.dump_model()

    def node(n):
        if "leaf_value" in n:
            return {"v": round(n["leaf_value"], 8)}
        assert n["decision_type"] == "<=", n["decision_type"]
        return {"f": n["split_feature"], "t": n["threshold"], "dl": n["default_left"],
                "l": node(n["left_child"]), "r": node(n["right_child"])}

    model = dict(features=b.feature_name(), objective=d["objective"],
                 trees=[node(t["tree_structure"]) for t in d["tree_info"]],
                 threshold=float((MODELS_DIR / "wesad_stress_threshold.txt").read_text().strip()))
    (OUT / "stress_model.json").write_text(json.dumps(model))
    return b


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    export_text()
    export_heads()
    check_stage2(export_stage2())
    export_stress()
    for f in sorted(OUT.iterdir()):
        print(f"{f.name}: {f.stat().st_size / 1e6:.2f} MB")


if __name__ == "__main__":
    main()
