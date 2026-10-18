"""
Caches a downsized copy of every image in dataset/skintone/manifest_clean.csv, so training
doesn't decode multi-megapixel phone photos (DermaCon-IN, SCIN) on every epoch.

Aspect ratio is PRESERVED: the shorter side is resized to SHORT_SIDE px and the longer
side scaled to match. The old pipeline's Resize((224, 224)) squashed 810x1080 phone
photos to a square, distorting lesion shape; the skin-tone scripts instead resize the
shorter side and crop (see skintone_transforms in train_skintone_cv.py).

Images are stored as high-quality JPEG (q=95). EXIF orientation is applied first, so
phone photos are upright.

Adds a path_small column to manifest_clean.csv.
"""
from concurrent.futures import ProcessPoolExecutor

import pandas as pd
from PIL import Image, ImageOps

from paths import ROOT, DATASET_DIR

OUT_DIR = DATASET_DIR / "skintone"
CACHE_DIR = OUT_DIR / "cache"
SHORT_SIDE = 320


def shrink(args):
    src, dst = args
    if dst.exists():
        return
    with Image.open(src) as im:
        im = ImageOps.exif_transpose(im).convert("RGB")
        w, h = im.size
        scale = SHORT_SIDE / min(w, h)
        if scale < 1:
            im = im.resize((round(w * scale), round(h * scale)), Image.BICUBIC)
        dst.parent.mkdir(parents=True, exist_ok=True)
        im.save(dst, "JPEG", quality=95)


def main():
    df = pd.read_csv(OUT_DIR / "manifest_clean.csv")
    dsts = [CACHE_DIR / s / (p.replace("/", "__").rsplit(".", 1)[0] + ".jpg")
            for s, p in zip(df["source"], df["path"])]
    with ProcessPoolExecutor() as ex:
        list(ex.map(shrink, [(ROOT / p, d) for p, d in zip(df["path"], dsts)], chunksize=32))
    df["path_small"] = [str(d.relative_to(ROOT)) for d in dsts]
    df.to_csv(OUT_DIR / "manifest_clean.csv", index=False)
    print(f"cached {len(df)} images under {CACHE_DIR}")


if __name__ == "__main__":
    main()
