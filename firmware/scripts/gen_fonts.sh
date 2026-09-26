#!/usr/bin/env bash
# Regenerates firmware/main/fonts/*.c — the UI fonts.
#
# WHY CUSTOM FONTS: LVGL's built-in Montserrat covers ASCII + "°" + "•" only.
# The daemon sends agent names, summaries and questions in the user's own
# language (é è à ç œ …, "Working…" with U+2026) and this UI draws "λ" and "·"
# — all of which rendered as blanks with the built-in fonts.
#
# Requirements: node + `npm i -g lv_font_conv@1.5.2`, curl.
# Sources (all freely redistributable, see fonts/README.md):
#   Montserrat-Medium.ttf                    SIL OFL 1.1   (LVGL scripts/built_in_font)
#   FontAwesome5-Solid+Brands+Regular.woff   SIL OFL 1.1   (LVGL scripts/built_in_font)
#   DejaVuSans(-Bold).ttf                    Bitstream Vera licence (λ only)
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=main/fonts
SRC="${FONT_SRC:-$(mktemp -d)}"
LVGL_RAW=https://raw.githubusercontent.com/lvgl/lvgl/release/v9.2/scripts/built_in_font

for f in Montserrat-Medium.ttf FontAwesome5-Solid+Brands+Regular.woff; do
  [ -f "$SRC/$f" ] || curl -fsSL -o "$SRC/$f" "$LVGL_RAW/$f"
done
DEJAVU="${DEJAVU_DIR:-/usr/share/fonts/truetype/dejavu}"
[ -f "$DEJAVU/DejaVuSans.ttf" ] || { echo "DejaVuSans.ttf not found in $DEJAVU (apt install fonts-dejavu-core)"; exit 1; }

# The LVGL built-in symbol set (LV_SYMBOL_*), so every LV_SYMBOL_ macro works
# with these fonts exactly as with lv_font_montserrat_*.
SYMS="61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650"

# Latin-1 + French typography + arrows. U+00B7 "·" is in 0xA0-0xFF.
TEXT="0x20-0x7E,0xA0-0xFF,0x152-0x153,0x178,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026,0x2039-0x203A,0x20AC,0x2190-0x2193"

gen() {  # name size text-ranges with-lambda-font
  local name=$1 size=$2 ranges=$3 lfont=$4
  lv_font_conv --no-compress --no-prefilter --bpp 4 --size "$size" \
    --font "$SRC/Montserrat-Medium.ttf" -r "$ranges" \
    --font "$DEJAVU/$lfont" -r 0x3BB \
    --font "$SRC/FontAwesome5-Solid+Brands+Regular.woff" -r "$SYMS" \
    --format lvgl --lv-include lvgl.h \
    --force-fast-kern-format -o "$OUT/$name.c"
  echo "generated $OUT/$name.c"
}

gen font_ui_14 14 "$TEXT" DejaVuSans.ttf
gen font_ui_20 20 "$TEXT" DejaVuSans.ttf
# 28 px: ASCII, "·", "…", the carousel chevrons "‹ ›", arrows.
gen font_ui_28 28 "0x20-0x7E,0xB7,0x2026,0x2039-0x203A,0x2190-0x2193" DejaVuSans-Bold.ttf

# Boot logo: a single big "λ".
lv_font_conv --no-compress --no-prefilter --bpp 4 --size 72 \
  --font "$DEJAVU/DejaVuSans-Bold.ttf" -r 0x3BB \
  --format lvgl --lv-include lvgl.h \
  -o "$OUT/font_logo_72.c"
echo "generated $OUT/font_logo_72.c"

# lv_font_conv 1.5.2 emits an LVGL-8 glyph cache that no longer exists in v9
# (lv_font_fmt_txt_glyph_cache_t / .cache were removed) — make it v8-only, the
# way LVGL 9's own built-in fonts do. Also strip absolute paths from the
# "Opts:" header so regeneration is byte-reproducible.
python3 - "$OUT" <<'PY'
import pathlib, re, sys
for p in sorted(pathlib.Path(sys.argv[1]).glob("font_*.c")):
    s = p.read_text()
    s = s.replace(
        "#if LV_VERSION_CHECK(8, 0, 0)\n/*Store all the custom data of the font*/\nstatic  lv_font_fmt_txt_glyph_cache_t cache;\n",
        "#if LVGL_VERSION_MAJOR == 8\nstatic lv_font_fmt_txt_glyph_cache_t cache;\n#endif\n#if LVGL_VERSION_MAJOR >= 8\n/*Store all the custom data of the font*/\n")
    s = s.replace("#if LV_VERSION_CHECK(8, 0, 0)\n    .cache = &cache\n", "#if LVGL_VERSION_MAJOR == 8\n    .cache = &cache\n")
    s = re.sub(r"--font \S*/([^/\s]+)", r"--font \1", s)
    s = re.sub(r"-o \S*/(font_[a-z0-9_]+\.c)", r"-o \1", s)
    assert "lv_font_fmt_txt_glyph_cache_t cache;\n#endif" in s or "cache" not in s, p
    p.write_text(s)
    print("patched", p)
PY
