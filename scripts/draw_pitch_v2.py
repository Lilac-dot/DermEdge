"""
Draws the pitch-deck diagrams for the v2 skin model (31 Oct 2026) and the band's data story:

    python scripts/draw_pitch_v2.py

  v2_0_full_system.png       the whole system: one CLIP pass, three heads; the band's sensors
  v2_1_old_vs_new.png        v1 (CLIP + ResNet18) vs v2 (one CLIP encoder, three heads)
  v2_2_stage1_rash.png       stage 1 in detail
  v2_3_stage2_eczema.png     stage 2 in detail, with the occlusion heatmap
  v2_4_device_tiers.png      which model runs on which device
  v2_5_band_data_loop.png    the band as a data-collection platform for doctors and future models
  v2_6_results_vs_market.png our AUCs next to published dermatology models

Same look as scripts/draw_architecture.py (its helpers are reused), 3200 x 1800 px. Numbers come
from experiments/web_heads_v2/summary.json, experiments/encoder_comparison/summary.json,
experiments/clip_severity_head/summary.json and docs/SENSOR_DATA.md.
"""
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch

from draw_architecture import INK, MUTED, OUT, arrow, box, canvas, legend, note, save, section

OURS, THEIRS = "#eb6834", "#2a78d6"  # validated pair (dataviz validate_palette.js, light)


def panel(ax, x, y, w, h, title):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0,rounding_size=18", fc="#fafafa", ec="#d0d4da", lw=1.6))
    ax.text(x + 28, y + 38, title, fontsize=15, fontweight="bold", color=INK, va="center")


def full_system():
    fig, ax = canvas("DermEdge: full system (v2)",
                     "One CLIP pass per photo feeds all three skin stages. Everything runs on the user's own device; "
                     "no photo is uploaded.")
    section(ax, 50, 160, "IMAGE PATH")
    box(ax, 50, 185, 220, 130, "input", "Photo", "phone or laptop", "camera")
    box(ax, 305, 185, 220, 130, "rule", "Quality check", "blur / too dark /", "too bright")
    box(ax, 560, 185, 300, 130, "frozen", "CLIP ViT-B/16", "one pass → 512-number", "embedding (8-bit, 90 MB)")
    box(ax, 920, 185, 300, 130, "trained", "Stage 1 head", "Normal skin vs rash", "+ skin check")
    box(ax, 1280, 185, 300, 130, "trained", "Stage 2 head", "Eczema vs look-alikes", "+ heatmap (laptop)")
    box(ax, 1640, 185, 310, 130, "trained", "Stage 3 head", "Severity 0-12", "(4 EASI signs)")
    arrow(ax, (270, 250), (305, 250))
    arrow(ax, (525, 250), (560, 250))
    arrow(ax, (860, 250), (920, 250))
    arrow(ax, (1220, 250), (1280, 250), label="rash", at=(1250, 168))
    arrow(ax, (1580, 250), (1640, 250), label="eczema", at=(1610, 168))
    ax.plot([710, 710, 1795], [315, 345, 345], color="#3a6fd8", lw=2, ls=(0, (2, 2)))
    for x in (1430, 1795):
        arrow(ax, (x, 345), (x, 315))
    ax.text(1095, 331, "same embedding, no second model", fontsize=10, style="italic", color=MUTED, va="center")
    box(ax, 305, 405, 220, 75, "output", "Ask for a retake")
    box(ax, 920, 405, 300, 75, "output", "No rash detected")
    box(ax, 1280, 405, 300, 88, "output", "Probably a look-alike", "→ see a doctor")
    box(ax, 1640, 405, 310, 88, "output", "Verdict + severity", "+ body-site history")
    arrow(ax, (415, 315), (415, 405), label="bad photo", at=(428, 375), ha="left")
    arrow(ax, (1070, 315), (1070, 405), label="no rash", at=(1083, 385), ha="left")
    arrow(ax, (1430, 360), (1430, 405))
    arrow(ax, (1795, 360), (1795, 405))

    section(ax, 50, 548, "DERMEDGE BAND + JOURNAL PATH")
    box(ax, 50, 575, 360, 82, "input", "ADXL345 accelerometer", "800 Hz, back of the hand")
    box(ax, 450, 575, 300, 82, "trained", "Scratch detector", "on the band, our recordings")
    box(ax, 50, 667, 360, 82, "input", "AD8232 ECG + LM35", "seated 1-2 min check; wrist temp")
    box(ax, 450, 667, 300, 82, "trained", "Stress model", "heart rate + HRV vs your calm")
    box(ax, 50, 759, 360, 82, "future", "Moisture patch (future)", "eczema vs own healthy skin")
    box(ax, 50, 851, 360, 82, "input", "Journal", "sleep, new product, stress")
    arrow(ax, (410, 616), (450, 616))
    arrow(ax, (410, 708), (450, 708))
    box(ax, 830, 660, 290, 160, "rule", "Trigger index", "noisy-OR of all triggers", "1 − Π(1 − tᵢ)")
    arrow(ax, (750, 616), (790, 616), (790, 690), (830, 690))
    arrow(ax, (750, 708), (830, 708))
    arrow(ax, (410, 800), (830, 800), dashed=True)
    arrow(ax, (410, 892), (790, 892), (790, 790), (830, 790))
    box(ax, 1220, 685, 330, 110, "rule", "Flare risk", "= image score × trigger index")
    arrow(ax, (1120, 740), (1220, 740))
    arrow(ax, (1795, 493), (1795, 560), (1385, 560), (1385, 685), label="image score", at=(1590, 542))
    box(ax, 1640, 640, 310, 100, "output", "Dashboard", "history · tips · doctor report")
    box(ax, 1640, 760, 310, 90, "output", "Alert on the band", "high risk")
    box(ax, 1640, 870, 310, 90, "output", "Doctor's data", "sensor timelines + CSV")
    arrow(ax, (1550, 740), (1595, 740), (1595, 690), (1640, 690))
    arrow(ax, (1550, 760), (1595, 760), (1595, 805), (1640, 805))
    arrow(ax, (975, 820), (975, 915), (1640, 915), label="every recording, stored on the device", at=(1310, 897))
    legend(ax)
    save(fig, "v2_0_full_system.png")


def old_vs_new():
    fig, ax = canvas("From v1 to v2: one encoder, three heads, tested on real-world photos",
                     "v1 scored well on atlas-style test photos. Testing on unseen phone and South Asian hospital photos "
                     "showed it didn't carry over, so we rebuilt it.")
    panel(ax, 40, 135, 935, 850, "v1  (25 Oct)")
    panel(ax, 1025, 135, 935, 850, "v2  (31 Oct)")
    # v1
    box(ax, 80, 215, 200, 110, "input", "Photo")
    box(ax, 330, 215, 280, 110, "frozen", "CLIP ViT-B/16", "frozen")
    box(ax, 660, 215, 280, 110, "trained", "Rash head", "US phone photos")
    box(ax, 330, 395, 280, 125, "trained", "ResNet18", "second model, 45 MB,", "atlas photos only")
    box(ax, 660, 395, 280, 125, "trained", "Eczema vs", "look-alikes", "+ Grad-CAM")
    box(ax, 660, 575, 280, 110, "trained", "Severity head", "on CLIP")
    arrow(ax, (280, 270), (330, 270))
    arrow(ax, (610, 270), (660, 270))
    arrow(ax, (180, 325), (180, 457), (330, 457))
    arrow(ax, (610, 457), (660, 457))
    arrow(ax, (590, 325), (590, 355), (955, 355), (955, 630), (940, 630))
    note(ax, 80, 735, 860, 220, "v1 in numbers",
         "2 image models · 135 MB download",
         "Stage 2: 81% on its own atlas test set,",
         "but it missed most eczema in real phone photos",
         "South Asian hospital rashes passed by stage 1: 33%",
         "Darker skin (V–VI): 67% of rashes caught")
    # v2
    box(ax, 1065, 215, 200, 110, "input", "Photo")
    box(ax, 1310, 215, 280, 140, "frozen", "CLIP ViT-B/16", "one pass, 512-number", "embedding")
    box(ax, 1640, 215, 280, 100, "trained", "Rash head", "+ South Asian photos")
    box(ax, 1640, 355, 280, 100, "trained", "Eczema head", "SCIN + SkinDisNet")
    box(ax, 1640, 495, 280, 100, "trained", "Severity head", "unchanged")
    box(ax, 1310, 425, 280, 110, "rule", "Occlusion heatmap", "laptop GPU")
    arrow(ax, (1265, 270), (1310, 270))
    arrow(ax, (1590, 265), (1640, 265))
    arrow(ax, (1615, 265), (1615, 405), (1640, 405))
    arrow(ax, (1615, 400), (1615, 545), (1640, 545))
    arrow(ax, (1640, 425), (1590, 480))
    note(ax, 1065, 655, 860, 300, "v2 in numbers",
         "1 image model · 90 MB · runs on laptops and phones",
         "Stage 1: AUC 0.92 on real phone photos",
         "South Asian hospital rashes passed: 33% → 84%",
         "Darker skin (V–VI): 67% → 86%, no calibration needed",
         "Stage 2: AUC 0.87 on South Asian hospital photos",
         "Matched two 2025 dermatology foundation models",
         "on unseen photos (CLIP 0.89 · DermLIP 0.91 · PanDerm 0.89)")
    legend(ax)
    save(fig, "v2_1_old_vs_new.png")


def stage1():
    fig, ax = canvas("Stage 1: is there a rash?",
                     "A frozen CLIP encoder turns the photo into 512 numbers; a skin check and a trained rash head read them.")
    box(ax, 50, 200, 240, 130, "input", "Photo", "after the quality check")
    box(ax, 330, 200, 260, 130, "rule", "Prepare", "shortest side 224 px,", "centre crop, normalise")
    box(ax, 630, 200, 330, 130, "frozen", "CLIP ViT-B/16", "8-bit, 90 MB, in the browser", "WebGPU / WebAssembly")
    box(ax, 1000, 200, 260, 130, "frozen", "Embedding", "512 numbers")
    arrow(ax, (290, 265), (330, 265))
    arrow(ax, (590, 265), (630, 265))
    arrow(ax, (960, 265), (1000, 265))
    box(ax, 1320, 160, 330, 120, "rule", "Skin check", "vs CLIP text prompts: skin,", "rash, body part, not skin")
    box(ax, 1320, 320, 330, 120, "trained", "Rash head", "logistic regression", "→ P(rash)")
    arrow(ax, (1260, 240), (1290, 240), (1290, 220), (1320, 220))
    arrow(ax, (1260, 290), (1290, 290), (1290, 380), (1320, 380))
    box(ax, 1720, 160, 240, 120, "output", "Not skin", "point the camera at skin")
    box(ax, 1720, 320, 240, 120, "output", "Rash?", "P ≥ 0.5 → stage 2")
    arrow(ax, (1650, 220), (1720, 220))
    arrow(ax, (1650, 380), (1720, 380))
    note(ax, 50, 520, 900, 300, "How the head was trained",
         "Data: SCIN (Google/Stanford, 1,849 real US phone photos)",
         "+ SkinDisNet (1,710 photos from two hospitals in Bangladesh)",
         "Started from the v1 head and pulled back toward it, so it keeps",
         "what it learned from our curated and webcam photos",
         "Each source weighs the same, so neither drowns the other",
         "Tested with 5-fold cross-validation, never on a training photo",
         "Calibrated per skin type in v1; v2 needs no calibration")
    note(ax, 1000, 520, 950, 300, "Results",
         "AUC 0.92 on real phone photos (SCIN, held out by patient)",
         "88% of rashes caught",
         "South Asian hospital rashes: 84% caught (v1: 33%)",
         "Fitzpatrick V–VI: 86% caught, the same as lighter skin",
         "(v1: 67%, or 83% with a skin-type setting)",
         "Runs in about 0.5 s on a laptop, 0.8 s on a phone")
    legend(ax)
    save(fig, "v2_2_stage1_rash.png")


def stage2():
    fig, ax = canvas("Stage 2: eczema or a look-alike?",
                     "A second trained head on the same embedding: no extra model to download. On laptops, an occlusion map "
                     "shows what the decision rested on.")
    box(ax, 50, 200, 280, 130, "frozen", "Embedding", "from stage 1's CLIP pass", "(512 numbers)")
    box(ax, 380, 200, 330, 130, "trained", "Eczema head", "logistic regression", "→ P(eczema)")
    box(ax, 770, 140, 330, 110, "output", "Eczema", "P ≥ 0.5 → stage 3 severity")
    box(ax, 770, 280, 330, 110, "output", "Look-alike", "e.g. fungal, scabies → doctor")
    arrow(ax, (330, 265), (380, 265))
    arrow(ax, (710, 240), (740, 240), (740, 195), (770, 195))
    arrow(ax, (710, 290), (740, 290), (740, 335), (770, 335))
    section(ax, 1170, 160, "HEATMAP (LAPTOPS WITH A FAST GPU)")
    box(ax, 1170, 185, 250, 110, "rule", "16 copies", "each with one square", "greyed out")
    box(ax, 1460, 185, 220, 110, "frozen", "CLIP", "one batched run")
    box(ax, 1720, 185, 230, 110, "rule", "Score drop", "per square")
    box(ax, 1460, 345, 490, 90, "output", "Heatmap over the photo", "where the eczema call came from")
    arrow(ax, (1420, 240), (1460, 240))
    arrow(ax, (1680, 240), (1720, 240))
    arrow(ax, (1835, 295), (1835, 345))
    note(ax, 50, 500, 900, 300, "How the head was trained",
         "Eczema: SCIN eczema photos + SkinDisNet atopic dermatitis and eczema",
         "Look-alikes: psoriasis, tinea, candidiasis, bites, scabies,",
         "lichen planus, drug rash, rosacea",
         "Each source weighs the same: US phone photos and",
         "South Asian hospital photos count equally",
         "Replaced v1's separate ResNet18 (45 MB), which was trained",
         "on atlas photos only")
    note(ax, 1000, 500, 950, 300, "Results",
         "AUC 0.87 on South Asian hospital photos (5-fold CV)",
         "Eczema caught 78% · look-alikes cleared 81%",
         "Scabies correctly cleared 89%",
         "Benchmarked against 2025 dermatology foundation models",
         "on photos none had seen: CLIP 0.89, DermLIP 0.91,",
         "PanDerm-DermLIP 0.89. We kept CLIP: as good, lighter,",
         "and freely licensed")
    legend(ax)
    save(fig, "v2_3_stage2_eczema.png")


def device_tiers():
    fig, ax = canvas("Sized to the device",
                     "The same stages wherever the hardware allows. Nothing is uploaded on any of them.")
    cols = [
        ("Laptop browser", "frozen", ["CLIP ViT-B/16, WebGPU", "All 3 stages + heatmap", "~0.5 s per check"], "live", False),
        ("Phone browser / app", "frozen", ["Same CLIP, WebAssembly", "All 3 stages", "~0.8 s per check"], "live", False),
        ("Phone, lighter model", "future", ["17 MB MobileNetV3 that", "copies CLIP's embedding,", "so every head still works"], "script ready", True),
        ("ESP32-CAM", "future", ["300 KB INT8 MobileNet", "rash screen on the chip", "97% on its test set"], "future scope", True),
    ]
    for i, (title, kind, lines, status, fut) in enumerate(cols):
        x = 50 + i * 480
        box(ax, x, 200, 440, 120, "input" if not fut else "future", title)
        box(ax, x, 360, 440, 200, kind, lines[0], *lines[1:])
        ax.text(x + 220, 600, status, ha="center", va="center", fontsize=13, fontweight="bold",
                color="#2e9e5b" if status == "live" else MUTED)
        arrow(ax, (x + 220, 320), (x + 220, 360), dashed=fut)
    note(ax, 50, 700, 1900, 160, "And on the band (ESP32)",
         "The scratch detector runs on the band itself: 800 Hz motion → scratch episodes, with nothing raw leaving the band.",
         "The website picks the tier on its own (phone or laptop, GPU or not); a failed load retries instead of breaking the app.")
    legend(ax)
    save(fig, "v2_4_device_tiers.png")


def band_data_loop():
    fig, ax = canvas("The DermEdge band: collecting the data eczema care is missing",
                     "No public dataset pairs these sensors with eczema. The band records it from day one, doctors use it "
                     "now, and it trains tomorrow's models.")
    section(ax, 50, 160, "ON THE BAND")
    box(ax, 50, 185, 330, 95, "input", "ADXL345", "scratching, 800 Hz")
    box(ax, 50, 295, 330, 95, "input", "AD8232 ECG", "seated check, both wrists")
    box(ax, 50, 405, 330, 95, "input", "LM35", "wrist skin temperature")
    box(ax, 430, 185, 330, 95, "trained", "Scratch episodes", "detected on the band")
    box(ax, 430, 295, 330, 95, "rule", "Heart rate + HRV", "from R-peaks")
    box(ax, 430, 405, 330, 95, "rule", "Temperature", "vs your calm baseline")
    for y in (232, 342, 452):
        arrow(ax, (380, y), (430, y))
    box(ax, 820, 280, 300, 125, "rule", "Bluetooth LE", "encrypted, passkey-paired", "buffers offline")
    for y in (232, 342, 452):
        arrow(ax, (760, y), (790, y), (790, 342), (820, 342))
    section(ax, 1180, 160, "THE LOOP")
    box(ax, 1180, 185, 360, 110, "output", "1. Record", "every session stored on the", "patient's device, with photos")
    box(ax, 1590, 185, 360, 110, "output", "2. Doctor reviews", "trends between visits,", "timelines + CSV export")
    box(ax, 1590, 400, 360, 110, "future", "3. Data pools", "with consent, across patients")
    box(ax, 1180, 400, 360, 110, "future", "4. Models learn", "flags drafted automatically;", "doctors validate quickly")
    arrow(ax, (1120, 342), (1150, 342), (1150, 240), (1180, 240))
    arrow(ax, (1540, 240), (1590, 240))
    arrow(ax, (1770, 295), (1770, 400), dashed=True)
    arrow(ax, (1590, 455), (1540, 455), dashed=True)
    arrow(ax, (1360, 400), (1360, 295), dashed=True)
    note(ax, 50, 590, 920, 300, "Why skin sensors can detect eczema: the Southampton study",
         "13 eczema patients, each measured on eczema skin and on",
         "their own healthy skin (Todorov, Univ. of Southampton, CC BY):",
         "· Moisture sensor read lower on eczema skin: 13 of 13",
         "· Corneometer hydration lower: 13 of 13 (by 12–55%)",
         "· Water loss (TEWL) higher: 7 of 7",
         "A paired rule (patch vs own healthy skin) flags all 13.",
         "Known biomarkers work; what's missing is everyday data.")
    note(ax, 1030, 590, 920, 300, "What the band already does",
         "Scratch detection on the band: 22 of 26 scratch clips found",
         "across 7 people, each tested by a model that never saw them",
         "ECG check: heart rate and HRV against your own calm",
         "baseline; LM35 wrist temperature alongside",
         "Live readings in the app; flare alerts on the band's screen",
         "Every recording kept for the doctor, never uploaded",
         "")
    legend(ax)
    save(fig, "v2_5_band_data_loop.png")


def results_vs_market():
    rows = [  # (label, detail, auc, ours)
        ("DermEdge stage 1: rash screen", "real US phone photos (SCIN), held out by patient", 0.92, True),
        ("DermEdge stage 2: eczema vs look-alikes", "South Asian hospital photos (SkinDisNet), 5-fold CV", 0.87, True),
        ("DermEdge's encoder (CLIP ViT-B/16)", "encoder benchmark: unseen South Asian photos", 0.890, True),
        ("DermLIP (2025, dermatology CLIP)", "same benchmark, run by us", 0.906, False),
        ("PanDerm-DermLIP (2025)", "same benchmark, run by us", 0.890, False),
        ("Google MedSigLIP (2025, 400M params)", "79 skin conditions, US tele-dermatology (published)", 0.881, False),
        ("Google Derm Foundation", "same 79-condition task (published)", 0.843, False),
    ]
    fig = plt.figure(figsize=(16, 9), dpi=200)
    fig.text(0.031, 0.945, "How DermEdge compares", fontsize=24, fontweight="bold", color=INK, va="center")
    fig.text(0.031, 0.905, "AUC (1.0 is perfect, 0.5 is chance). DermEdge also scores severity, runs on a phone, and tracks the "
                           "band's data, which none of these do.", fontsize=12.5, color=MUTED, va="center")
    ax = fig.add_axes([0.36, 0.17, 0.58, 0.66])
    ys = list(range(len(rows)))[::-1]
    for y, (label, detail, auc, ours) in zip(ys, rows):
        ax.barh(y, auc, height=0.56, color=OURS if ours else THEIRS, edgecolor="white", linewidth=2)
        ax.text(auc + 0.012, y, f"{auc:.2f}", va="center", ha="left", fontsize=14, fontweight="bold", color=INK)
        fig.text(0.345, 0.17 + 0.66 * (y + 0.5) / len(rows) + 0.012, label, ha="right", va="center", fontsize=13,
                 fontweight="bold" if ours else "normal", color=INK)
        fig.text(0.345, 0.17 + 0.66 * (y + 0.5) / len(rows) - 0.018, detail, ha="right", va="center", fontsize=10.5, color=MUTED)
    ax.set_xlim(0, 1.0)
    ax.set_ylim(-0.6, len(rows) - 0.4)
    ax.set_yticks([])
    ax.set_xticks([0, 0.25, 0.5, 0.75, 1.0])
    ax.tick_params(axis="x", colors=MUTED, labelsize=11, length=0)
    ax.grid(axis="x", color="#e5e7eb", lw=1)
    ax.set_axisbelow(True)
    for s in ("top", "right", "left"):
        ax.spines[s].set_visible(False)
    ax.spines["bottom"].set_color("#d0d4da")
    for i, (c, t) in enumerate(((OURS, "DermEdge"), (THEIRS, "Published dermatology models"))):
        x = 0.36 + i * 0.16
        fig.patches.append(FancyBboxPatch((x, 0.065), 0.016, 0.026, boxstyle="round,pad=0,rounding_size=0.004",
                                          transform=fig.transFigure, fc=c, ec="none"))
        fig.text(x + 0.022, 0.078, t, fontsize=12, color=INK, va="center")
    fig.text(0.031, 0.03, "Google's numbers are from the MedGemma technical report (2025) on a different, multi-class task: "
                          "shown for scale, not as a head-to-head. DermLIP and PanDerm were run by us on the same photos.",
             fontsize=10, color=MUTED, va="center")
    fig.savefig(OUT / "v2_6_results_vs_market.png", dpi=200, facecolor="white")
    plt.close(fig)
    print("wrote", OUT / "v2_6_results_vs_market.png")


if __name__ == "__main__":
    full_system()
    old_vs_new()
    stage1()
    stage2()
    device_tiers()
    band_data_loop()
    results_vs_market()
