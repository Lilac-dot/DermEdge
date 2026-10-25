#!/bin/sh
# Renders every band screen with the real ui.c into test/preview.png (3x, with labels)
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
SYSROOT=
for sdk in /Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk /Library/Developer/CommandLineTools/SDKs/MacOSX15.sdk; do
    [ -d "$sdk" ] && SYSROOT="-isysroot $sdk" && break
done
cc $SYSROOT -std=c11 -Wall -Werror -o "$OUT/preview" preview.c ../main/ui.c ../main/ui_flow.c ../main/display.c ../main/fonts.c ../main/art.c -lm
"$OUT/preview" "$OUT"
python3 - "$OUT" <<'PY'
import sys
from pathlib import Path
from PIL import Image, ImageDraw
files = sorted(Path(sys.argv[1]).glob("*.ppm"))
S, cols = 3, 4
w, h = 160 * S, 128 * S
sheet = Image.new("RGB", (cols * (w + 16) + 16, ((len(files) + cols - 1) // cols) * (h + 40) + 16), "white")
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    x, y = 16 + (i % cols) * (w + 16), 16 + (i // cols) * (h + 40)
    sheet.paste(Image.open(f).resize((w, h), Image.NEAREST), (x, y))
    d.text((x, y + h + 6), f.stem.split("_", 1)[1].replace("_", " "), fill="black")
sheet.save("preview.png")
print("wrote test/preview.png")
PY
rm -rf "$OUT"
