"""
Tier 1 of the hackathon device: a tiny "normal skin vs rash" CNN for the ESP32-CAM.
If it says rash, the phone runs the full eczema-vs-look-alikes model and the severity
model, offline.

Run with the TensorFlow venv (TFLite Micro needs a TFLite model):
    .venv-tf/bin/python scripts/train_esp32_rash_gate.py

Data (label 1 = rash / skin condition, 0 = normal skin)
  normal  - MegPaulson/Healthy_Skin (HF), the 40% least rash-like by CLIP
            (prepare_megpaulson_healthy.py; the set is full of cuts, moles and bites)
          - Kaggle "Human Skin Diseases" Normal: the 416x416 skin close-ups with
            skin fraction >= 0.8 (the rest of that class is stock photos / animals)
          - SCIN healthy cases (download_scin_healthy.py), minus the 3 with visible lesions
  rash    - curated Stage B train + val (eczema + 7 look-alikes, DermNet-style)
          - SCIN eczema + look-alikes from the skin-tone DEVELOPMENT split
  The curated test split and dataset/skintone/manifest_test.csv are never read.

Held-out test: 25% of SCIN cases (healthy and condition, split by case). Both classes are
real consumer phone photos from the same source, so this is the honest number; the
mixed-source validation split only drives early stopping.

Model: Keras MobileNetV2 alpha=0.35 at 96x96 (ImageNet weights, ReLU6 only -- the SE /
hard-swish MobileNetV3 collapsed under INT8 in this project's compression study).
Rescaling is inside the model, so the ESP32 feeds raw 0-255 RGB.

Writes experiments/esp32_rash_gate/:
  rash_gate_int8.tflite, rash_gate_model_data.cc/.h (C array for TFLite Micro),
  test_predictions.csv, summary.json
"""
import json

import numpy as np
import pandas as pd
import tensorflow as tf
from PIL import Image
from sklearn.metrics import roc_auc_score
from sklearn.model_selection import GroupShuffleSplit

from paths import ROOT, DATASET_DIR, SKINDISEASE_DIR

OUT_DIR = ROOT / "experiments" / "esp32_rash_gate"
CACHE_DIR = OUT_DIR / "cache"
SEED = 42
IMG = 96
CACHE_SIDE = 128
BATCH = 64
MEGPAULSON_KEEP = 0.4
TEST_FRAC = 0.25
LESION_VISIBLE = {"SCIN_-6477152086839235973", "SCIN_1554139664304435205",
                  "SCIN_4898834119557476781"}


def skin_fraction(path):
    im = Image.open(path).convert("RGB")
    im.thumbnail((128, 128))
    ycbcr = np.asarray(im.convert("YCbCr"), dtype=np.int16)
    cb, cr = ycbcr[..., 1], ycbcr[..., 2]
    return float(((cr >= 133) & (cr <= 173) & (cb >= 77) & (cb <= 127)).mean())


def build_manifest():
    parts = []

    mp = pd.read_csv(DATASET_DIR / "healthy" / "megpaulson_rows.csv")
    mp = mp[mp["clip_p_rash"].rank(pct=True) < MEGPAULSON_KEEP]
    parts.append(pd.DataFrame(dict(path=mp["path_small"], label=0, source="megpaulson",
                                   group=mp["path_small"])))

    kag = []
    for split in ("train", "valid", "test"):
        for p in sorted((SKINDISEASE_DIR / split / "Normal").iterdir()):
            with Image.open(p) as im:
                if im.size == (416, 416) and skin_fraction(p) >= 0.8:
                    kag.append(str(p.relative_to(ROOT)))
    parts.append(pd.DataFrame(dict(path=kag, label=0, source="kaggle_normal", group=kag)))

    sh = pd.read_csv(DATASET_DIR / "healthy" / "scin_healthy_rows.csv")
    sh = sh[~sh["group_id"].isin(LESION_VISIBLE)]
    parts.append(pd.DataFrame(dict(path=sh["path"], label=0, source="scin", group=sh["group_id"])))

    cur = pd.concat([pd.read_csv(SKINDISEASE_DIR / f"manifest_curated_v3_{s}.csv")
                     for s in ("train", "val")], ignore_index=True)
    parts.append(pd.DataFrame(dict(path=cur["path"], label=1, source="curated", group=cur["path"])))

    st = pd.concat([pd.read_csv(DATASET_DIR / "skintone" / f"manifest_{s}.csv")
                    for s in ("train", "val")], ignore_index=True)
    st = st[st["source"] == "SCIN"]
    parts.append(pd.DataFrame(dict(path=st["path"], label=1, source="scin", group=st["split_group"])))

    df = pd.concat(parts, ignore_index=True)
    df["split"] = "train"
    scin = df.index[df["source"] == "scin"]
    for label in (0, 1):
        idx = scin[df.loc[scin, "label"] == label]
        gss = GroupShuffleSplit(1, test_size=TEST_FRAC, random_state=SEED)
        _, te = next(gss.split(idx, groups=df.loc[idx, "group"]))
        df.loc[idx[te], "split"] = "test"
    rest = df.index[df["split"] == "train"]
    gss = GroupShuffleSplit(1, test_size=0.15, random_state=SEED)
    _, va = next(gss.split(rest, df.loc[rest, "label"], groups=df.loc[rest, "group"]))
    df.loc[rest[va], "split"] = "val"
    return df


def build_manifest_v1():
    """The original v1 Eczema-vs-Normal data and splits (dataset/manifest_{train,val,test}.csv),
    as asked for the hackathon demo. Known shortcut (docs/PROJECT_HISTORY.md 1.1): the Normal
    images are stock portraits and studio hand photos, so brightness/background alone
    separates the classes -- expect high test accuracy that won't carry over to real skin.
    813 of the 1,329 Normal images were recovered from public mirrors
    (dataset/healthy/v1_normal_rows.csv); the rest are missing on this machine."""
    parts = []
    for split in ("train", "val", "test"):
        m = pd.read_csv(DATASET_DIR / f"manifest_{split}.csv")
        m = m[m["class"] == "Eczema"]
        rel = m["path"].str.replace("\\", "/", regex=False).str.replace(
            "C:/Users/tishy/Documents/Honors/", "", regex=False)
        parts.append(pd.DataFrame(dict(path=rel, label=1, source="eczema", group=rel, split=split)))
    n = pd.read_csv(DATASET_DIR / "healthy" / "v1_normal_rows.csv")
    parts.append(pd.DataFrame(dict(path=n["path"], label=0, source="normal", group=n["path"],
                                   split=n["split"])))
    # Own-camera examples saved with the e/n keys of webcam_eczema_demo.py: 80% train,
    # 20% val (never test, so the original test numbers stay comparable).
    rng = np.random.RandomState(SEED)
    for label, folder in ((1, "eczema"), (0, "normal")):
        caps = sorted((ROOT / "experiments" / "webcam_captures" / folder).glob("*.jpg"))
        if caps:
            rel = [str(p.relative_to(ROOT)) for p in caps]
            split = np.where(rng.rand(len(rel)) < 0.8, "train", "val")
            parts.append(pd.DataFrame(dict(path=rel, label=label, source=f"webcam_{folder}",
                                           group=rel, split=split)))
            print(f"added {len(rel)} own-camera {folder} examples")
    return pd.concat(parts, ignore_index=True)


def cache_images(df):
    """Shrinks every image once (EXIF-upright, shorter side CACHE_SIDE, JPEG) so the TF
    pipeline only ever decodes small JPEGs, whatever the source format."""
    from PIL import ImageOps
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    out = []
    for p in df["path"]:
        dst = CACHE_DIR / (p.replace("/", "__").rsplit(".", 1)[0] + ".jpg")
        if not dst.exists():
            with Image.open(ROOT / p) as im:
                im = ImageOps.exif_transpose(im).convert("RGB")
                w, h = im.size
                s = CACHE_SIDE / min(w, h)
                im = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.BICUBIC)
                im.save(dst, "JPEG", quality=92)
        out.append(str(dst))
    return out


def decode(path):
    return tf.io.decode_jpeg(tf.io.read_file(path), channels=3)


def camera_like(img):
    """Training augmentation: random crop/zoom + flips + the colour, blur and noise
    problems of a cheap OV2640 sensor."""
    scale = tf.random.uniform([], 0.55, 1.0)
    side = tf.cast(tf.cast(tf.reduce_min(tf.shape(img)[:2]), tf.float32) * scale, tf.int32)
    img = tf.image.random_crop(img, [side, side, 3])
    img = tf.image.resize(img, [IMG, IMG])
    img = tf.image.random_flip_left_right(img)
    img = tf.image.random_flip_up_down(img)
    img = tf.image.random_brightness(img, 40.0)
    img = tf.image.random_contrast(img, 0.7, 1.3)
    img = tf.image.random_saturation(img, 0.8, 1.2)
    if tf.random.uniform([]) < 0.3:
        img = tf.nn.avg_pool2d(img[None], 3, 1, "SAME")[0]
    img = img + tf.random.normal(tf.shape(img), stddev=tf.random.uniform([], 0.0, 8.0))
    return tf.clip_by_value(img, 0.0, 255.0)


def center(img):
    side = tf.reduce_min(tf.shape(img)[:2])
    img = tf.image.resize_with_crop_or_pad(img, side, side)
    return tf.image.resize(img, [IMG, IMG])


def dataset(df, train):
    ds = tf.data.Dataset.from_tensor_slices((df["cache"].values, df["label"].values.astype(np.float32),
                                             df["weight"].values.astype(np.float32)))
    if train:
        ds = ds.shuffle(len(df), seed=SEED, reshuffle_each_iteration=True)
    aug = camera_like if train else center
    ds = ds.map(lambda p, y, w: (aug(tf.cast(decode(p), tf.float32)), y, w),
                num_parallel_calls=tf.data.AUTOTUNE)
    return ds.batch(BATCH).prefetch(tf.data.AUTOTUNE)


ARCHS = {"mnv1_025": (96, 0.25), "mnv1_050": (128, 0.5), "mnv2_035": (96, 0.35)}


def build_model(arch):
    """mnv1_025: MobileNetV1 alpha 0.25, the architecture of TFLite Micro's ESP32
    person-detection example -- no residual adds, quantizes cleanly.
    mnv2_035: first attempt; INT8 post-training quantization cut its held-out AUC from
    0.75 to 0.57 (error builds up from the first depthwise layer onward)."""
    inp = tf.keras.Input((IMG, IMG, 3))
    x = tf.keras.layers.Rescaling(1 / 127.5, offset=-1)(inp)
    if arch.startswith("mnv1"):
        base = tf.keras.applications.MobileNet((IMG, IMG, 3), alpha=ARCHS[arch][1],
                                               include_top=False, weights="imagenet")
    else:
        base = tf.keras.applications.MobileNetV2((IMG, IMG, 3), alpha=0.35, include_top=False,
                                                 weights="imagenet")
    x = base(x)
    x = tf.keras.layers.GlobalAveragePooling2D()(x)
    x = tf.keras.layers.Dropout(0.3)(x)
    out = tf.keras.layers.Dense(1, activation="sigmoid")(x)
    return tf.keras.Model(inp, out), base


def weights(df):
    """Each class weighs the same in total, and within a class each source weighs the same,
    so no single source (e.g. 2,300 MegPaulson images) defines 'normal'."""
    w = pd.Series(1.0, index=df.index)
    for label, d in df.groupby("label"):
        per_source = d.groupby("source").size()
        w[d.index] = 1.0 / (d["source"].map(per_source) * len(per_source))
    return (w / w.mean()).values


def fit(model, train_ds, val_ds, lr, epochs):
    model.compile(tf.keras.optimizers.Adam(lr), "binary_crossentropy",
                  weighted_metrics=[], metrics=[tf.keras.metrics.AUC(name="auc")])
    stop = tf.keras.callbacks.EarlyStopping("val_auc", mode="max", patience=4,
                                            restore_best_weights=True)
    return model.fit(train_ds, validation_data=val_ds, epochs=epochs, callbacks=[stop], verbose=2)


def to_int8(model, rep_paths):
    def rep():
        for p in rep_paths:
            yield [center(tf.cast(decode(p), tf.float32))[None]]
    # from_keras_model crashes on Keras 3 / TF 2.16 ("missing attribute 'value'"), so go
    # through an exported SavedModel instead.
    saved = OUT_DIR / "saved_model"
    model.export(str(saved))
    conv = tf.lite.TFLiteConverter.from_saved_model(str(saved))
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = rep
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    return conv.convert()


def tflite_predict(blob, paths):
    it = tf.lite.Interpreter(model_content=blob)
    it.allocate_tensors()
    i, o = it.get_input_details()[0], it.get_output_details()[0]
    (si, zi), (so, zo) = i["quantization"], o["quantization"]
    out = []
    for p in paths:
        x = center(tf.cast(decode(p), tf.float32)).numpy()
        it.set_tensor(i["index"], np.clip(np.round(x / si + zi), -128, 127).astype(np.int8)[None])
        it.invoke()
        out.append((it.get_tensor(o["index"]).astype(np.float32)[0, 0] - zo) * so)
    return np.array(out)


def metrics(y, p, thr):
    pred = p >= thr
    return dict(auc=round(float(roc_auc_score(y, p)), 4), threshold=round(float(thr), 4),
                sensitivity=round(float(pred[y == 1].mean()), 4),
                specificity=round(float((~pred[y == 0]).mean()), 4),
                accuracy=round(float((pred == (y == 1)).mean()), 4),
                balanced_accuracy=round(float((pred[y == 1].mean() + (~pred[y == 0]).mean()) / 2), 4))


def write_c_array(blob, name):
    hex_rows = [", ".join(f"0x{b:02x}" for b in blob[i:i + 12]) for i in range(0, len(blob), 12)]
    (OUT_DIR / f"{name}.h").write_text(
        f"#pragma once\n#include <cstdint>\n\nextern const unsigned char {name}[];\n"
        f"extern const unsigned int {name}_len;\n")
    (OUT_DIR / f"{name}.cc").write_text(
        f'#include "{name}.h"\n\nalignas(16) const unsigned char {name}[] = {{\n  '
        + ",\n  ".join(hex_rows) + f"\n}};\nconst unsigned int {name}_len = {len(blob)};\n")


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-train", action="store_true",
                    help="reuse rash_gate_fp32.keras and only redo threshold/export/test")
    ap.add_argument("--arch", default="mnv1_025", choices=list(ARCHS))
    ap.add_argument("--data", default="rash", choices=["rash", "v1"],
                    help="rash: normal vs any skin condition (SCIN test); "
                         "v1: the original Eczema-vs-Normal data and splits")
    args = ap.parse_args()
    global OUT_DIR, IMG
    IMG = ARCHS[args.arch][0]
    prefix = "esp32_eczema_v1" if args.data == "v1" else "esp32_rash_gate"
    OUT_DIR = OUT_DIR.parent / f"{prefix}_{args.arch}"
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tf.random.set_seed(SEED)
    np.random.seed(SEED)
    df = build_manifest_v1() if args.data == "v1" else build_manifest()
    df["cache"] = cache_images(df)
    df.to_csv(OUT_DIR / "manifest.csv", index=False)
    print(pd.crosstab([df["split"], df["source"]], df["label"]).to_string())

    tr, va, te = (df[df["split"] == s].copy() for s in ("train", "val", "test"))
    tr["weight"], va["weight"], te["weight"] = weights(tr), weights(va), 1.0
    train_ds, val_ds = dataset(tr, True), dataset(va, False)

    if args.skip_train:
        model = tf.keras.models.load_model(OUT_DIR / "rash_gate_fp32.keras")
    else:
        model, base = build_model(args.arch)
        base.trainable = False
        fit(model, train_ds, val_ds, 1e-3, 10)
        base.trainable = True
        for layer in base.layers[:-30]:
            layer.trainable = False
        fit(model, train_ds, val_ds, 1e-4, 20)
        model.save(OUT_DIR / "rash_gate_fp32.keras")

    # Threshold: the validation point where 95% of rashes are caught -- tier 1 should rarely
    # miss a rash; the phone model cleans up false alarms. v1's classes are balanced, so it
    # keeps the plain 0.5.
    p_val = model.predict(dataset(va, False), verbose=0).ravel()
    thr = 0.5 if args.data == "v1" else float(np.quantile(p_val[va["label"].values == 1], 0.05))

    blob = to_int8(model, list(tr.sample(300, random_state=SEED)["cache"]))
    (OUT_DIR / "rash_gate_int8.tflite").write_bytes(blob)
    write_c_array(blob, "rash_gate_model_data")

    y = te["label"].values
    p32 = model.predict(dataset(te, False), verbose=0).ravel()
    p8 = tflite_predict(blob, te["cache"])
    te["prob_fp32"], te["prob_int8"] = p32, p8
    te.to_csv(OUT_DIR / "test_predictions.csv", index=False)
    summary = dict(
        n=pd.crosstab(df["split"], df["label"]).to_dict(),
        test="original v1 Eczema-vs-Normal test split" if args.data == "v1"
        else "held-out SCIN cases only (healthy vs any condition, phone photos)",
        tflite_bytes=len(blob), threshold=round(thr, 4),
        fp32=metrics(y, p32, thr), int8=metrics(y, p8, thr),
    )
    if args.data == "v1":
        with open(OUT_DIR / "summary.json", "w") as f:
            json.dump(summary, f, indent=2)
        print(json.dumps(summary, indent=2))
        return
    ecz = te["path"].isin(set(pd.concat([pd.read_csv(DATASET_DIR / "skintone" / f"manifest_{s}.csv")
                                         for s in ("train", "val")]).query("disease_class == 'Eczema'")["path"]))
    keep = (te["label"] == 0) | ecz
    summary["int8_eczema_vs_normal"] = metrics(y[keep.values], p8[keep.values], thr)
    with open(OUT_DIR / "summary.json", "w") as f:
        json.dump(summary, f, indent=2)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
