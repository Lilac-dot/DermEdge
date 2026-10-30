"""
Phone tier of the website's skin model: a small MobileNetV3 trained to give the same 512-number
image embedding as the laptop's CLIP ViT-B/16 (knowledge distillation). Because the embedding is
the same, the rash head, the severity head, the skin-type offsets and the "is this skin" check in
web/models/heads.json and clip_text.json work on it unchanged: nothing is relabelled.

Runs on Google Colab (Runtime > Change runtime type > T4 GPU) or any CUDA machine:
    !git clone https://github.com/Lilac-dot/DermEdge.git && cd DermEdge
    !pip -q install timm onnx onnxruntime transformers
    !python scripts/train_phone_student.py --download-scin --out /content/student

Training images: every SCIN photo (about 10,000 real phone photos of skin, public bucket, no
login), plus any folders passed with --extra (e.g. the Kaggle Human Skin Diseases set). No
labels are needed: the teacher's embedding is the target. 10% of SCIN cases are held out, and
the report compares the student with the teacher on them through the real heads.

Writes to --out:
    phone_vision.onnx   input pixel_values [1,3,224,224] (CLIP-normalised, like engine.mjs),
                        output image_embeds [1,512]; copy it to web/models/
    summary.json        held-out agreement with the teacher, size, settings
    student.pt          PyTorch weights
"""
import argparse
import io
import json
import math
import random
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image

BUCKET = "https://storage.googleapis.com/dx-scin-public-data/"
ROOT = Path(__file__).resolve().parent.parent
CLIP_MEAN = torch.tensor([0.48145466, 0.4578275, 0.40821073]).view(1, 3, 1, 1)
CLIP_STD = torch.tensor([0.26862954, 0.26130258, 0.27577711]).view(1, 3, 1, 1)
SIDE = 256  # images are stored as 256x256 centre crops; 224 crops are taken from them


def square(img, side=SIDE):
    """Shortest side to `side`, centre crop. A 224 resize of this whole square shows exactly
    what engine.mjs shows the model (shortest side to 224, centre crop)."""
    img = img.convert("RGB")
    s = side / min(img.size)
    img = img.resize((max(side, round(img.width * s)), max(side, round(img.height * s))), Image.BICUBIC)
    l, t = (img.width - side) // 2, (img.height - side) // 2
    return np.asarray(img.crop((l, t, l + side, t + side)), dtype=np.uint8)


def download_scin(out_dir):
    """All SCIN images, saved as 256x256 JPEGs named <case_id>_<n>.jpg."""
    import pandas as pd
    out_dir.mkdir(parents=True, exist_ok=True)
    cases = pd.read_csv(BUCKET + "dataset/scin_cases.csv", dtype={"case_id": str})
    jobs = [(r[f"image_{i}_path"], out_dir / f"{r['case_id']}_{i}.jpg")
            for r in cases.to_dict("records") for i in (1, 2, 3) if isinstance(r.get(f"image_{i}_path"), str)]

    def one(job):
        src, dst = job
        if dst.exists():
            return True
        try:
            with urllib.request.urlopen(BUCKET + src, timeout=60) as r:
                Image.fromarray(square(Image.open(io.BytesIO(r.read())))).save(dst, quality=95)
            return True
        except Exception as e:
            print("  failed", dst.name, e)
            return False

    t = time.time()
    with ThreadPoolExecutor(32) as ex:
        ok = sum(ex.map(one, jobs))
    print(f"SCIN: {ok} of {len(jobs)} images in {time.time() - t:.0f} s")


def load_images(paths):
    def one(p):
        try:
            return square(Image.open(p))
        except Exception:
            return None
    with ThreadPoolExecutor(16) as ex:
        arr = list(ex.map(one, paths))
    keep = [i for i, a in enumerate(arr) if a is not None]
    return np.stack([arr[i] for i in keep]), [paths[i] for i in keep]


class Teacher(nn.Module):
    """openai/clip-vit-base-patch16 (what the website runs today) or an open_clip hub model
    such as hf-hub:redlessone/DermLIP_ViT-B-16. Takes 0-1 RGB, applies its own normalisation."""
    def __init__(self, name):
        super().__init__()
        if name.startswith("hf-hub:"):
            import open_clip
            self.m, _, pre = open_clip.create_model_and_transforms(name)
            norm = [t for t in pre.transforms if t.__class__.__name__ == "Normalize"][0]
            mean, std, self.kind = norm.mean, norm.std, "open_clip"
        else:
            from transformers import CLIPVisionModelWithProjection
            self.m = CLIPVisionModelWithProjection.from_pretrained(name)
            mean, std, self.kind = CLIP_MEAN.flatten().tolist(), CLIP_STD.flatten().tolist(), "hf"
        self.register_buffer("mean", torch.tensor(mean).view(1, 3, 1, 1))
        self.register_buffer("std", torch.tensor(std).view(1, 3, 1, 1))

    @torch.no_grad()
    def forward(self, x01):
        x = (x01 - self.mean) / self.std
        e = self.m.encode_image(x) if self.kind == "open_clip" else self.m(pixel_values=x).image_embeds
        return F.normalize(e.float(), dim=-1)


class Student(nn.Module):
    def __init__(self, arch, dim):
        super().__init__()
        import timm
        self.body = timm.create_model(arch, pretrained=True, num_classes=0)
        self.proj = nn.Linear(self.body.num_features, dim)

    def forward(self, pixel_values):  # CLIP-normalised input, like the browser sends
        return self.proj(self.body(pixel_values))


def augment(batch_u8, device, train):
    """uint8 NHWC 256x256 -> 0-1 NCHW 224x224 on the GPU. Training: random crops of 70-100% of
    the square, flips, slight brightness/contrast; colour is left alone (redness matters)."""
    x = batch_u8.to(device, non_blocking=True).permute(0, 3, 1, 2).float() / 255
    if not train:
        return F.interpolate(x, size=224, mode="bicubic", align_corners=False, antialias=True).clamp(0, 1)
    out = []
    for img in x:
        s = random.randint(int(0.7 * SIDE), SIDE)
        t, l = random.randint(0, SIDE - s), random.randint(0, SIDE - s)
        img = img[:, t:t + s, l:l + s]
        if random.random() < 0.5:
            img = img.flip(2)
        if random.random() < 0.3:
            img = img.flip(1)
        out.append(F.interpolate(img[None], size=224, mode="bicubic", align_corners=False, antialias=True)[0])
    x = torch.stack(out)
    b = 1 + (torch.rand(len(x), 1, 1, 1, device=device) - 0.5) * 0.3
    c = 1 + (torch.rand(len(x), 1, 1, 1, device=device) - 0.5) * 0.3
    m = x.mean(dim=(1, 2, 3), keepdim=True)
    return ((x - m) * c + m * b).clamp(0, 1)


def head_report(t_emb, s_emb):
    """Teacher vs student through the website's own heads (only meaningful for the CLIP teacher)."""
    heads = json.loads((ROOT / "web/models/heads.json").read_text())
    text = json.loads((ROOT / "web/models/clip_text.json").read_text())
    w, b = np.array(heads["rash"]["w"]), heads["rash"]["b"]
    sw, sb = np.array(heads["severity"]["w"]), heads["severity"]["b"]
    T = {k: np.array(text[k]) for k in ("normal", "rash", "no_skin", "body")}

    def run(e):
        e = e / np.linalg.norm(e, axis=1, keepdims=True)
        p = 1 / (1 + np.exp(-(e @ w + b)))
        lg = {k: 100 * e @ v for k, v in T.items()}
        skin = lg["no_skin"] <= np.maximum.reduce([lg["normal"], lg["rash"], lg["body"]]) + 4
        return p, skin, np.clip(e @ sw + sb, 0, 12)

    pt, kt, st = run(t_emb)
    ps, ks, ss = run(s_emb)
    pos = pt >= 0.5
    from sklearn.metrics import roc_auc_score
    return dict(
        rash_decision_agreement=float(np.mean((pt >= 0.5) == (ps >= 0.5))),
        rash_prob_mean_abs_diff=float(np.mean(np.abs(pt - ps))),
        student_auc_vs_teacher_decision=float(roc_auc_score(pos, ps)) if 0 < pos.sum() < len(pos) else None,
        skin_check_agreement=float(np.mean(kt == ks)),
        severity_mean_abs_diff_points=float(np.mean(np.abs(st - ss)[pos])) if pos.any() else None,
        n_teacher_rash=int(pos.sum()), n=int(len(pt)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "experiments/phone_student"))
    ap.add_argument("--download-scin", action="store_true")
    ap.add_argument("--scin-dir", default=str(ROOT / "dataset/SCIN/student_256"))
    ap.add_argument("--extra", nargs="*", default=[], help="more image folders (searched recursively)")
    ap.add_argument("--teacher", default="openai/clip-vit-base-patch16")
    ap.add_argument("--student", default="mobilenetv3_large_100")
    ap.add_argument("--epochs", type=int, default=25)
    ap.add_argument("--batch", type=int, default=128)
    ap.add_argument("--lr", type=float, default=2e-3)
    args = ap.parse_args()
    out = Path(args.out); out.mkdir(parents=True, exist_ok=True)
    dev = "cuda" if torch.cuda.is_available() else "cpu"
    random.seed(0); np.random.seed(0); torch.manual_seed(0)

    scin = Path(args.scin_dir)
    if args.download_scin:
        download_scin(scin)
    exts = {".jpg", ".jpeg", ".png", ".webp", ".bmp"}
    scin_paths = sorted(p for p in scin.glob("*") if p.suffix.lower() in exts)
    extra = sorted(p for d in args.extra for p in Path(d).rglob("*") if p.suffix.lower() in exts)
    # hold out 10% of SCIN cases (all images of a case on the same side)
    cases = sorted({p.stem.rsplit("_", 1)[0] for p in scin_paths})
    held = set(random.Random(1).sample(cases, max(1, len(cases) // 10))) if cases else set()
    val_paths = [p for p in scin_paths if p.stem.rsplit("_", 1)[0] in held]
    train_paths = [p for p in scin_paths if p.stem.rsplit("_", 1)[0] not in held] + extra
    print(f"train {len(train_paths)} images ({len(extra)} extra), held out {len(val_paths)} from {len(held)} SCIN cases")
    t = time.time()
    Xtr, _ = load_images(train_paths); Xva, _ = load_images(val_paths)
    print(f"loaded in {time.time() - t:.0f} s: {Xtr.nbytes / 1e9:.1f} GB")
    Xtr, Xva = torch.from_numpy(Xtr), torch.from_numpy(Xva)

    teacher = Teacher(args.teacher).to(dev).eval().half()
    student = Student(args.student, 512).to(dev)
    mean, std = CLIP_MEAN.to(dev), CLIP_STD.to(dev)
    opt = torch.optim.AdamW(student.parameters(), lr=args.lr, weight_decay=0.02)
    steps = args.epochs * math.ceil(len(Xtr) / args.batch)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=steps, pct_start=0.1)
    scaler = torch.amp.GradScaler(enabled=dev == "cuda")

    def embed_all(X, model_fn):
        outs = []
        for i in range(0, len(X), 256):
            x = augment(X[i:i + 256], dev, False)
            with torch.no_grad(), torch.autocast(dev, enabled=dev == "cuda"):
                outs.append(F.normalize(model_fn(x).float(), dim=-1).cpu())
        return torch.cat(outs).numpy()

    t_val = embed_all(Xva, lambda x: teacher(x.half()))
    for ep in range(args.epochs):
        student.train(); perm = torch.randperm(len(Xtr)); tot = 0; t = time.time()
        for i in range(0, len(perm), args.batch):
            x = augment(Xtr[perm[i:i + args.batch]], dev, True)
            with torch.no_grad():
                target = teacher(x.half())
            with torch.autocast(dev, enabled=dev == "cuda"):
                s = F.normalize(student((x - mean) / std).float(), dim=-1)
                loss = (1 - (s * target).sum(-1)).mean()
            opt.zero_grad(set_to_none=True); scaler.scale(loss).backward(); scaler.step(opt); scaler.update(); sched.step()
            tot += loss.item() * len(x)
        student.eval()
        s_val = embed_all(Xva, lambda x: student((x - mean) / std))
        cos = float(np.mean(np.sum(s_val * t_val, axis=1)))
        print(f"epoch {ep + 1}/{args.epochs}  train cos {1 - tot / len(Xtr):.4f}  held-out cos {cos:.4f}  ({time.time() - t:.0f} s)")

    torch.save(student.state_dict(), out / "student.pt")
    student = student.float().cpu().eval()
    onnx_path = out / "phone_vision.onnx"
    torch.onnx.export(student, torch.zeros(1, 3, 224, 224), str(onnx_path), input_names=["pixel_values"],
                      output_names=["image_embeds"], opset_version=17, dynamo=False)
    import onnxruntime as ort
    xs = ((augment(Xva[:8], "cpu", False) - CLIP_MEAN) / CLIP_STD).numpy()
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    onnx_out = np.concatenate([sess.run(None, {"pixel_values": xs[i:i + 1]})[0] for i in range(len(xs))])
    with torch.no_grad():
        torch_out = student(torch.from_numpy(xs)).numpy()
    summary = dict(
        teacher=args.teacher, student=args.student, epochs=args.epochs,
        n_train=len(Xtr), n_heldout=len(Xva), heldout_cases=len(held),
        heldout_mean_cosine=cos, onnx_mb=round(onnx_path.stat().st_size / 1e6, 1),
        onnx_matches_torch_max_abs=float(np.abs(onnx_out - torch_out).max()))
    if args.teacher == "openai/clip-vit-base-patch16":
        summary["through_website_heads"] = head_report(t_val, s_val)
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
