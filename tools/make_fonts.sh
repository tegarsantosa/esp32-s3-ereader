#!/bin/sh
# Regenerates main/fonts/*.c from tools/DejaVuSans.ttf (or a font you pass in).
#   ./tools/make_fonts.sh                 # uses DejaVuSans.ttf
#   ./tools/make_fonts.sh MyFont.ttf      # any TTF/OTF you are allowed to embed
set -e
cd "$(dirname "$0")"
FONT="${1:-DejaVuSans.ttf}"
CFLAGS=$(pkg-config --cflags --libs freetype2 2>/dev/null || echo "-I/opt/homebrew/include/freetype2 -L/opt/homebrew/lib -lfreetype")
cc -O2 fontgen.c -o fontgen $CFLAGS
for px in 10 12 14 16 18 20 24 28; do
    ./fontgen "$FONT" $px font_sans_$px > ../main/fonts/font_sans_$px.c
done
echo "Fonts written to main/fonts/"
