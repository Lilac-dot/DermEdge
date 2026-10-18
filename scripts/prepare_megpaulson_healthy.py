"""
Extracts the Hugging Face "MegPaulson/Healthy_Skin" set (6,039 skin close-ups, every caption
"Healthy human skin", no license on the card) into the 320-px JPEG cache, drops
near-duplicates, and scores every image with the CLIP zero-shot healthy-vs-rash prompts
from healthy_gate_probe.py (AUC 0.89 on SCIN healthy vs condition).

Spot-checking showed the set is noisy -- moles, stitches, bruises, insect bites and the odd
psoriasis-like plaque sit among the clean skin -- so the most rash-like images by CLIP are
dropped (keep_rash_prob_below).

Writes:
  dataset/healthy/megpaulson/cache/<n>.jpg
  dataset/healthy/megpaulson_rows.csv  (path_small, ahash, clip_p_rash, keep)
"""
import io

import numpy as np
import pandas as pd
import torch
from PIL import Image

from healthy_gate_probe import ZERO_SHOT_PROMPTS
from paths import ROOT, DATASET_DIR

SRC_DIR = DATASET_DIR / "healthy" / "megpaulson"
CACHE_DIR = SRC_DIR / "cache"
ROWS_PATH = DATASET_DIR / "healthy" / "megpaulson_rows.csv"
SHORT_SIDE = 320
KEEP_QUANTILE = 0.8  # keep the 80% least rash-like images


def ahash(im):
    a = np.asarray(im.convert("L").resize((8, 8)), dtype=np.float32)
    return "".join("1" if v else "0" for v in (a > a.mean()).ravel())


def extract():
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    rows, n = [], 0
    for shard in sorted(SRC_DIR.glob("*.parquet")):
        for rec in pd.read_parquet(shard)["image"]:
            im = Image.open(io.BytesIO(rec["bytes"])).convert("RGB")
            w, h = im.size
            scale = SHORT_SIDE / min(w, h)
            if scale < 1:
                im = im.resize((round(w * scale), round(h * scale)), Image.BICUBIC)
            out = CACHE_DIR / f"{n:05d}.jpg"
            im.save(out, "JPEG", quality=95)
            rows.append(dict(path_small=str(out.relative_to(ROOT)), ahash=ahash(im),
                             width=w, height=h))
            n += 1
    return pd.DataFrame(rows)


@torch.no_grad()
def clip_p_rash(paths, device):
    from transformers import CLIPModel, CLIPProcessor
    name = "openai/clip-vit-base-patch16"
    model = CLIPModel.from_pretrained(name).to(device).eval()
    proc = CLIPProcessor.from_pretrained(name)
    text = []
    for label in (0, 1):
        t = model.get_text_features(**proc(text=ZERO_SHOT_PROMPTS[label], return_tensors="pt",
                                           padding=True).to(device))
        t = (t / t.norm(dim=-1, keepdim=True)).mean(0)
        text.append(t / t.norm())
    text = torch.stack(text)
    out = []
    for i in range(0, len(paths), 64):
        ims = [Image.open(ROOT / p).convert("RGB") for p in paths[i:i + 64]]
        f = model.get_image_features(**proc(images=ims, return_tensors="pt").to(device))
        f = f / f.norm(dim=-1, keepdim=True)
        out.append(torch.softmax(100 * f @ text.T, dim=1)[:, 1].float().cpu().numpy())
    return np.concatenate(out)


def main():
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    df = extract()
    n0 = len(df)
    df = df.drop_duplicates("ahash").reset_index(drop=True)
    print(f"extracted {n0}, {len(df)} after dropping near-duplicates")
    df["clip_p_rash"] = clip_p_rash(list(df["path_small"]), device)
    cut = float(df["clip_p_rash"].quantile(KEEP_QUANTILE))
    df["keep"] = df["clip_p_rash"] < cut
    df.to_csv(ROWS_PATH, index=False)
    print(f"keeping {int(df['keep'].sum())} images with CLIP P(rash) < {cut:.3f} -> {ROWS_PATH}")


if __name__ == "__main__":
    main()
