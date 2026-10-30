"""
Draws the system diagrams in docs/architecture/ that changed with the current plan:

    python scripts/draw_architecture.py

  0_full_system.png          the whole system (phone/laptop camera, the DermEdge band)
  7_old_vs_new.png           the skin model's first version vs now (31 Oct)
  1_stage1_clip_rash_v2.png  stage 1 now; 2_stage2_clip_head_v2.png stage 2 now
  4_wearable_flare_risk.png  wearable triggers and flare risk (the band's GSR stress model)
  6_band_scratch.png         scratch detection on the band

Same look as the stage 1-3 diagrams (3200 x 1800 px): grey inputs, blue frozen models, orange
models we trained, green fixed steps, purple outputs, and dashed grey for future scope.
Coordinates are on a 2000 x 1125 grid, y down.
"""
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

OUT = Path(__file__).resolve().parents[1] / "docs" / "architecture"
W, H = 2000, 1125
INK, MUTED, LINE = "#1f2328", "#4b5563", "#4a4f57"
KINDS = {  # face, edge
    "input": ("#f2f3f5", "#8a8f98"),
    "frozen": ("#e6eefc", "#3a6fd8"),
    "trained": ("#fff0d9", "#e08a00"),
    "rule": ("#e3f4ea", "#2e9e5b"),
    "output": ("#f1e8ff", "#8b5cf6"),
    "future": ("#ffffff", "#9aa1ab"),
}
LEGEND = [("input", "Input / data"), ("frozen", "Frozen pretrained (not changed)"),
          ("trained", "Trained by the team"), ("rule", "Fixed step / rule"), ("output", "Output"),
          ("future", "Future scope / planned")]


def canvas(title, subtitle):
    fig = plt.figure(figsize=(16, 9), dpi=200)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_xlim(0, W)
    ax.set_ylim(H, 0)
    ax.axis("off")
    ax.text(50, 52, title, fontsize=24, fontweight="bold", color=INK, va="center")
    ax.text(50, 96, subtitle, fontsize=12.5, color=MUTED, va="center")
    return fig, ax


def box(ax, x, y, w, h, kind, title, *lines):
    face, edge = KINDS[kind]
    future = kind == "future"
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=14", fc=face, ec=edge,
                                lw=2, ls=(0, (5, 3)) if future else "-"))
    rows = [(title, True)] + [(t, False) for t in lines]
    step = 25
    top = y + h / 2 - step * (len(rows) - 1) / 2
    for i, (t, bold) in enumerate(rows):
        ax.text(x + w / 2, top + i * step, t, ha="center", va="center", fontsize=12 if bold else 10.5,
                fontweight="bold" if bold else "normal", color=MUTED if future and not bold else INK)


def arrow(ax, *pts, dashed=False, label=None, at=None, ha="center"):
    """Orthogonal polyline through pts, with a head on the last segment."""
    ls = (0, (5, 3)) if dashed else "-"
    for (x0, y0), (x1, y1) in zip(pts[:-2], pts[1:-1]):
        ax.plot([x0, x1], [y0, y1], color=LINE, lw=2, ls=ls, solid_capstyle="butt")
    ax.add_patch(FancyArrowPatch(pts[-2], pts[-1], arrowstyle="-|>,head_length=7,head_width=3.5", color=LINE,
                                 lw=2, ls="-", shrinkA=0, shrinkB=0, mutation_scale=1.6))
    if label:
        ax.text(*at, label, fontsize=10, style="italic", color=MUTED, ha=ha, va="center")


def section(ax, x, y, text):
    ax.text(x, y, text, fontsize=10.5, fontweight="bold", color="#8a8f98", va="center")


def note(ax, x, y, w, h, title, *lines):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=14", fc="white", ec="#a5abb3",
                                lw=1.6, ls=(0, (5, 3))))
    ax.text(x + 25, y + 34, title, fontsize=12, fontweight="bold", color=INK, va="center")
    for i, t in enumerate(lines):
        ax.text(x + 25, y + 66 + i * 30, t, fontsize=11, color=INK, va="center")


def legend(ax, y=1075):
    x = 50
    for kind, text in LEGEND:
        face, edge = KINDS[kind]
        ax.add_patch(FancyBboxPatch((x, y - 11), 26, 22, boxstyle="round,pad=0,rounding_size=5", fc=face, ec=edge,
                                    lw=1.6, ls=(0, (3, 2)) if kind == "future" else "-"))
        t = ax.text(x + 38, y, text, fontsize=10.5, color=MUTED, va="center")
        x += 38 + len(text) * 9.6 + 50


def save(fig, name):
    fig.savefig(OUT / name, dpi=200, facecolor="white")
    plt.close(fig)
    print("wrote", OUT / name)


def full_system():
    fig, ax = canvas("DermEdge: full system",
                     "Everything runs on the user's own device: the website (in the browser) or the laptop hub. "
                     "No photo is uploaded.")
    section(ax, 50, 163, "IMAGE PATH")
    box(ax, 50, 187, 263, 138, "input", "Photo", "phone or laptop camera", "(website or hub)")
    box(ax, 350, 187, 250, 138, "rule", "Quality check", "blur / too dark /", "too bright")
    box(ax, 650, 187, 300, 138, "trained", "Stage 1", "Normal skin vs rash", "CLIP (frozen) + linear head")
    box(ax, 1012, 187, 300, 138, "trained", "Stage 2", "Eczema vs look-alikes", "head on the CLIP embedding")
    box(ax, 1400, 187, 288, 138, "trained", "Stage 3", "Severity 0-12", "CLIP embedding → ridge")
    arrow(ax, (313, 256), (350, 256))
    arrow(ax, (600, 256), (650, 256))
    arrow(ax, (950, 256), (1012, 256), label="rash", at=(981, 234))
    arrow(ax, (1312, 256), (1400, 256), label="eczema", at=(1356, 234))
    box(ax, 350, 412, 250, 75, "output", "Ask for a retake")
    box(ax, 650, 412, 300, 75, "output", "No rash detected")
    box(ax, 1012, 412, 300, 88, "output", "Probably not eczema", "→ see a doctor")
    arrow(ax, (475, 325), (475, 412), label="bad photo", at=(490, 369), ha="left")
    arrow(ax, (800, 325), (800, 412), label="not skin / P(rash) < 0.5", at=(815, 369), ha="left")
    arrow(ax, (1162, 325), (1162, 412), label="look-alike", at=(1177, 369), ha="left")
    box(ax, 50, 380, 263, 120, "future", "Future scope", "ESP32-CAM screen", "300 KB INT8 model")
    arrow(ax, (181, 380), (181, 325), dashed=True)

    section(ax, 50, 560, "WEARABLE + JOURNAL PATH")
    box(ax, 50, 590, 330, 80, "input", "DermEdge band", "ADXL345 800 Hz, back of the hand")
    box(ax, 420, 590, 300, 80, "trained", "Scratch detector", "trained on our recordings")
    box(ax, 50, 680, 330, 80, "input", "ECG check (AD8232)", "+ LM35 skin temperature")
    box(ax, 420, 680, 300, 80, "trained", "Stress model", "HR / HRV vs your calm baseline")
    box(ax, 50, 770, 330, 80, "future", "Moisture patch (future)", "eczema skin vs healthy skin")
    box(ax, 50, 860, 330, 80, "input", "Journal", "stress, sleep, new product")
    arrow(ax, (380, 630), (420, 630))
    arrow(ax, (380, 720), (420, 720))
    box(ax, 800, 675, 287, 162, "rule", "Trigger index", "noisy-OR of all triggers", "1 − Π(1 − tᵢ)")
    arrow(ax, (720, 630), (770, 630), (770, 697), (800, 697))
    arrow(ax, (720, 720), (800, 720))
    arrow(ax, (380, 795), (800, 795), dashed=True)
    arrow(ax, (380, 900), (770, 900), (770, 818), (800, 818))

    box(ax, 1200, 700, 350, 112, "rule", "Flare risk", "= image score × trigger index")
    arrow(ax, (1087, 756), (1200, 756))
    arrow(ax, (1544, 325), (1544, 587), (1375, 587), (1375, 700))
    ax.text(1562, 532, "image score = P(eczema) × severity / 12", fontsize=10, style="italic", color=MUTED,
            va="center")
    box(ax, 1625, 700, 325, 112, "output", "Dashboard", "history · alerts & tips", "· doctor report")
    arrow(ax, (1550, 756), (1625, 756))
    box(ax, 1625, 860, 325, 90, "output", "Alert on the band", "high risk; double tap dismisses")
    arrow(ax, (943, 837), (943, 905), (1625, 905), label="high trigger index", at=(1290, 887))
    legend(ax)
    save(fig, "0_full_system.png")


def wearable():
    fig, ax = canvas("Stage 4: Wearable triggers and flare risk",
                     "Stress and scratching are learned from data; the other triggers and the fusion are rules, "
                     "because no dataset pairs photos with wearable data.")
    box(ax, 50, 187, 300, 125, "input", "Band GSR", "skin conductance")
    box(ax, 390, 187, 170, 125, "rule", "30 s", "windows")
    box(ax, 600, 187, 250, 125, "rule", "6 EDA features", "mean, std, min, max,", "range, slope")
    box(ax, 890, 187, 230, 125, "rule", "Relative to", "personal baseline")
    box(ax, 1160, 187, 250, 125, "trained", "Logistic regression", "(TRAINED)", "→ P(stress)")
    box(ax, 1450, 187, 300, 125, "rule", "Stress trigger", "smoothed over ~90 s,", "scaled to 0-1")
    for x0, x1 in ((350, 390), (560, 600), (850, 890), (1120, 1160), (1410, 1450)):
        arrow(ax, (x0, 250), (x1, 250))
    box(ax, 890, 375, 230, 112, "rule", "First 3 calm", "windows (90 s)", "→ personal baseline")
    arrow(ax, (1005, 375), (1005, 312))
    box(ax, 50, 375, 300, 112, "future", "Skin-temp thermistor", "planned second input,", "once the model is retrained")
    arrow(ax, (350, 431), (475, 431), (475, 312), dashed=True)

    box(ax, 50, 563, 412, 75, "input", "Band scratch episodes", "(full trigger at 30 in 8 hours)")
    box(ax, 50, 663, 412, 75, "input", "Moisture patch → hydration deficit", "(full trigger at 8% drier)")
    box(ax, 50, 763, 412, 75, "input", "Journal", "stress, sleep hours, new soap / product")
    box(ax, 950, 587, 325, 175, "rule", "Trigger index", "noisy-OR: 1 − Π(1 − tᵢ)", "any one high trigger → high")
    arrow(ax, (1600, 312), (1600, 525), (1112, 525), (1112, 587), label="stress", at=(1356, 508))
    arrow(ax, (462, 600), (950, 600))
    arrow(ax, (462, 700), (950, 700))
    arrow(ax, (462, 800), (900, 800), (900, 743), (950, 743))
    box(ax, 1375, 600, 325, 150, "rule", "Flare risk", "= image score × trigger index", "(no eczema in photo → ~0)")
    box(ax, 1750, 600, 225, 150, "input", "Image score", "P(eczema) ×", "severity / 12")
    arrow(ax, (1275, 675), (1375, 675))
    arrow(ax, (1750, 675), (1700, 675))
    box(ax, 1375, 825, 325, 125, "output", "Alert + tips", "high ≥ 0.6 · moderate ≥ 0.3", "one tip per active trigger",
        "high → shown on the band")
    arrow(ax, (1112, 762), (1112, 887), (1375, 887), label="trigger index", at=(1243, 869))
    note(ax, 50, 870, 875, 160, "How the stress model was trained",
         "WESAD, 15 people. Calm before and after the stress task are both negatives,",
         "so time in the session can't pass for stress. Leave-one-person-out:",
         "90% accuracy · 73% of stress caught · 97% / 93% of calm cleared · weak for 4 of 15")
    legend(ax)
    save(fig, "4_wearable_flare_risk.png")


def band():
    fig, ax = canvas("The DermEdge band: scratch detection on the hand",
                     "Runs on the band's ESP32. Only scratch episodes and movement summaries leave it, "
                     "over encrypted Bluetooth.")
    box(ax, 50, 200, 250, 125, "input", "ADXL345", "800 Hz, on the back of", "the scratching hand")
    box(ax, 340, 200, 230, 125, "rule", "Every second", "the last 2 s of motion")
    box(ax, 610, 200, 300, 125, "rule", "Motion features", "strength · rhythm (2.5-8 /s)", "power 2-5 vs 5-20 Hz")
    box(ax, 950, 200, 250, 125, "trained", "Logistic model", "(TRAINED on our", "recordings)")
    box(ax, 1240, 200, 180, 125, "rule", "Scratch?", "P ≥ 0.7")
    box(ax, 1460, 200, 290, 125, "rule", "Episode", "starts after 2 scratchy s,", "ends after 4 calm s")
    for x0, x1 in ((300, 340), (570, 610), (910, 950), (1200, 1240), (1420, 1460)):
        arrow(ax, (x0, 262), (x1, 262))
    box(ax, 1460, 430, 290, 100, "rule", "Outbox", "buffers ~1 hour offline")
    box(ax, 1060, 430, 320, 100, "rule", "Bluetooth LE", "6-digit passkey, encrypted")
    box(ax, 650, 430, 330, 100, "output", "Website or laptop hub", "sessions · scratching trigger")
    box(ax, 240, 430, 330, 100, "output", "Band screen", "state, timer, flare alerts")
    arrow(ax, (1605, 325), (1605, 430))
    arrow(ax, (1460, 480), (1380, 480))
    arrow(ax, (1060, 480), (980, 480))
    arrow(ax, (650, 480), (570, 480), label="alerts", at=(610, 462))
    note(ax, 50, 620, 1250, 250, "First results and next tests",
         "One wearer, 16 clips: tested on the round it didn't see, it found 4 of 4 scratch clips",
         "with 0 false episodes in the 12 others (typing, waving, phone, rubbing, marching, rest)",
         "The band's C code and the Python model agree on all 284 one-second windows",
         "Skin-vibration features (ADAM study) tried: the ADXL345's noise buries them, so motion only",
         "Next: 83 clips from 6 volunteers for a leave-one-person-out test, then the volunteer day",
         )
    note(ax, 1340, 620, 610, 250, "Also on the band",
         "Touch pad: hold 2 s to start or stop",
         "Double tap: dismiss an alert",
         "Limit: it sees only the hand",
         "wearing it",
         "")
    legend(ax)
    save(fig, "6_band_scratch.png")


def old_vs_new():
    fig, ax = canvas("The skin model: first version vs now",
                     "Tested on photos it had never seen, the first stage 2 was at chance on real phone photos. "
                     "We rebuilt it around one shared CLIP pass.")
    section(ax, 50, 160, "FIRST VERSION (25 OCT)")
    box(ax, 50, 185, 250, 130, "input", "Photo")
    box(ax, 350, 185, 330, 130, "trained", "Stage 1: CLIP + rash head", "trained on US phone photos")
    box(ax, 730, 185, 330, 130, "trained", "Stage 2: separate ResNet18", "trained on atlas photos", "+ Grad-CAM heatmap")
    box(ax, 1110, 185, 330, 130, "trained", "Stage 3: severity head", "on the CLIP embedding")
    box(ax, 1490, 185, 460, 130, "output", "Two models to download", "135 MB", "skin type needed a manual offset")
    for x0, x1 in ((300, 350), (680, 730), (1060, 1110), (1440, 1490)):
        arrow(ax, (x0, 250), (x1, 250))
    note(ax, 50, 345, 1900, 120, "What testing on unseen photos showed",
         "Stage 2 scored 81% on atlas photos but caught 6-10% of eczema on real phone photos · the rash screen passed",
         "only 1 in 3 rashes from South Asian hospitals · darker skin (V-VI) caught 67% of rashes vs 84% on lighter skin")

    section(ax, 50, 540, "NOW (31 OCT)")
    box(ax, 50, 565, 250, 150, "input", "Photo", "phone or laptop")
    box(ax, 350, 565, 300, 150, "frozen", "CLIP ViT-B/16", "one pass, 8-bit, 90 MB", "→ 512-number embedding")
    box(ax, 740, 565, 380, 95, "trained", "Rash head", "US + South Asian photos")
    box(ax, 740, 675, 380, 95, "trained", "Eczema vs look-alikes head", "US + South Asian photos")
    box(ax, 740, 785, 380, 95, "trained", "Severity head", "EASI signs, 0-12")
    arrow(ax, (300, 640), (350, 640))
    arrow(ax, (650, 612), (740, 612))
    arrow(ax, (650, 640), (695, 640), (695, 722), (740, 722))
    arrow(ax, (695, 722), (695, 832), (740, 832))
    box(ax, 1200, 565, 750, 315, "output", "Results",
        "One model to download, 90 MB, phone and laptop",
        "South Asian rashes caught: 33% → 84%",
        "Eczema vs look-alikes on South Asian photos: AUC 0.87",
        "Darker skin (V-VI): 86% of rashes caught, no offset needed",
        "Occlusion heatmap on laptops",
        "Same heads run on any device")
    arrow(ax, (1120, 722), (1200, 722))
    legend(ax)
    save(fig, "7_old_vs_new.png")


def stage1_v2():
    fig, ax = canvas("Stage 1: is there a rash?",
                     "One frozen CLIP pass gives a 512-number embedding; a trained head turns it into P(rash).")
    box(ax, 50, 200, 250, 140, "input", "Photo", "centre crop 224 px")
    box(ax, 340, 200, 250, 140, "rule", "Quality check", "blur · too dark ·", "too bright → retake")
    box(ax, 630, 200, 300, 140, "frozen", "CLIP ViT-B/16", "image encoder", "8-bit weights, in the browser")
    box(ax, 970, 200, 260, 140, "input", "Embedding", "512 numbers")
    box(ax, 1270, 140, 330, 120, "rule", "Skin check", "CLIP text prompts:", "is this skin at all?")
    box(ax, 1270, 290, 330, 120, "trained", "Rash head", "logistic regression", "(TRAINED)")
    box(ax, 1650, 290, 300, 120, "output", "P(rash)", "≥ 0.5 → stage 2")
    for x0, x1 in ((300, 340), (590, 630), (930, 970)):
        arrow(ax, (x0, 270), (x1, 270))
    arrow(ax, (1230, 250), (1250, 250), (1250, 200), (1270, 200))
    arrow(ax, (1230, 290), (1250, 290), (1250, 350), (1270, 350))
    arrow(ax, (1600, 350), (1650, 350))
    note(ax, 50, 480, 920, 260, "Trained on",
         "SCIN: 1,849 real US phone photos (rashes + healthy skin)",
         "SkinDisNet: 1,710 photos from 2 hospitals in Bangladesh",
         "Started from our first head and pulled back toward it,",
         "so it keeps what it learned from earlier photo sets")
    note(ax, 1030, 480, 920, 260, "Results (5-fold cross-validation, unseen people)",
         "AUC 0.92 on real phone photos · 88% of rashes caught",
         "South Asian hospital rashes caught: 84% (was 33%)",
         "Darker skin (Fitzpatrick V-VI): 86% caught, AUC 0.95,",
         "the same as lighter skin, with no extra calibration")
    legend(ax)
    save(fig, "1_stage1_clip_rash_v2.png")


def stage2_v2():
    fig, ax = canvas("Stage 2: eczema or a look-alike?",
                     "A second trained head on the same CLIP embedding: no second model to download.")
    box(ax, 50, 200, 280, 140, "input", "Embedding", "from stage 1's CLIP pass")
    box(ax, 380, 200, 360, 140, "trained", "Eczema head", "logistic regression (TRAINED)", "each dataset weighted equally")
    box(ax, 790, 200, 300, 140, "output", "P(eczema)", "≥ 0.5 → severity", "< 0.5 → see a doctor")
    arrow(ax, (330, 270), (380, 270))
    arrow(ax, (740, 270), (790, 270))
    box(ax, 1150, 200, 380, 140, "rule", "Occlusion heatmap", "cover each of 16 squares,", "see how far P(eczema) moves")
    box(ax, 1580, 200, 370, 140, "output", "Heatmap", "what the decision rested on", "(laptops with a fast GPU)")
    arrow(ax, (1090, 270), (1150, 270))
    arrow(ax, (1530, 270), (1580, 270))
    note(ax, 50, 420, 920, 300, "Look-alikes it separates eczema from",
         "Psoriasis · tinea (ringworm) · scabies · candidiasis",
         "insect bites · lichen planus · drug rash · rosacea",
         "",
         "Trained on SCIN (US phone photos) + SkinDisNet",
         "(atopic dermatitis and eczema vs scabies and tinea)")
    note(ax, 1030, 420, 920, 300, "Results on South Asian hospital photos",
         "AUC 0.87 (5-fold cross-validation)",
         "78% of eczema caught · 81% of look-alikes cleared",
         "Encoder head-to-head, same heads and photos:",
         "CLIP 0.89 · DermLIP 0.91 · DermLIP-PanDerm 0.89",
         "→ on par with dermatology foundation models")
    legend(ax)
    save(fig, "2_stage2_clip_head_v2.png")


if __name__ == "__main__":
    full_system()
    wearable()
    band()
    old_vs_new()
    stage1_v2()
    stage2_v2()
