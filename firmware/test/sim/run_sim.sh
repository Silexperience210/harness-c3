#!/usr/bin/env bash
# Host UI simulator: builds the REAL ui.c + cable_client.c + LVGL 9.2.2 for the
# host, runs the scripted session in sim_main.c (every outbound frame is
# checked), and writes a PNG per screen into $OUT (default test/sim/out).
#
#   bash firmware/test/sim/run_sim.sh            # needs gcc, python3 (+Pillow for PNGs)
#   LVGL_DIR=/path/to/lvgl-9.2.2 bash ...        # reuse an LVGL checkout
set -euo pipefail
cd "$(dirname "$0")"
SIM=$PWD
FW=$(cd ../.. && pwd)
MAIN=$FW/main
OUT=${OUT:-$SIM/out}
BUILD=${BUILD:-$SIM/.build}
LVGL_VERSION=v9.2.2

# LVGL: the managed component if idf.py already fetched it, else a pinned clone.
if [ -z "${LVGL_DIR:-}" ]; then
  if [ -f "$FW/managed_components/lvgl__lvgl/lvgl.h" ]; then
    LVGL_DIR=$FW/managed_components/lvgl__lvgl
  else
    LVGL_DIR=$SIM/.lvgl
    [ -f "$LVGL_DIR/lvgl.h" ] || git clone -q --depth 1 -b "$LVGL_VERSION" https://github.com/lvgl/lvgl.git "$LVGL_DIR"
  fi
fi
grep -q "LVGL_VERSION_MINOR 2" "$LVGL_DIR/lv_version.h" || { echo "LVGL at $LVGL_DIR is not 9.2.x"; exit 1; }

mkdir -p "$BUILD" "$OUT"
# SIM_DEVICE=1: 32-bit pointers and the device's LVGL pool size (read from
# sdkconfig.defaults), so the pool high-water mark printed at the end is what
# the ESP32-C3 will see. Needs gcc-multilib.
ARCH_FLAGS=""
if [ "${SIM_DEVICE:-0}" = 1 ]; then
  KB=$(sed -n 's/^CONFIG_LV_MEM_SIZE_KILOBYTES=//p' "$FW/sdkconfig.defaults")
  ARCH_FLAGS="-m32 -DLV_MEM_SIZE=(${KB:-48}*1024U)"
  BUILD=$BUILD-device
fi
CFLAGS="-std=gnu11 -O1 -g $ARCH_FLAGS -DLV_CONF_INCLUDE_SIMPLE -I$SIM -I$LVGL_DIR"
LIB=$BUILD/liblvgl_sim.a
STAMP=$BUILD/lvgl.stamp
KEY="$( (cat "$SIM/lv_conf.h" "$LVGL_DIR/lv_version.h"; echo "$ARCH_FLAGS") | sha1sum | cut -c1-12)"
if [ ! -f "$LIB" ] || [ "$(cat "$STAMP" 2>/dev/null)" != "$KEY" ]; then
  echo "== building LVGL for the host (once) =="
  rm -rf "$BUILD/lvgl" && mkdir -p "$BUILD/lvgl"
  find "$LVGL_DIR/src" -name '*.c' | sort > "$BUILD/lvgl.list"
  i=0
  while read -r src; do
    i=$((i+1))
    gcc $CFLAGS -w -c "$src" -o "$BUILD/lvgl/$i.o"
  done < "$BUILD/lvgl.list"
  ar rcs "$LIB" "$BUILD"/lvgl/*.o
  echo "$KEY" > "$STAMP"
fi

echo "== building the simulator =="
WARN="-Wall -Wextra -Werror -Wno-unused-parameter -Wno-missing-field-initializers"
INC="-I$SIM/stubs -I$SIM -I$MAIN -I$FW/test/host/cjson -I$LVGL_DIR"
gcc $CFLAGS $WARN $INC -DHARNESS_UI_SIM -DCABLE_HOST_TEST \
  "$SIM/sim_main.c" "$SIM/sim_platform.c" \
  "$MAIN/ui.c" "$MAIN/cable_client.c" "$MAIN/cable_frame.c" \
  "$FW/test/host/cjson/cJSON.c" \
  "$MAIN"/fonts/font_*.c \
  "$LIB" -lm -o "$BUILD/sim"

echo "== running the scripted session =="
rm -f "$OUT"/*.ppm "$OUT"/*.png
"$BUILD/sim" "$OUT"

# PPM → PNG, plus a contact sheet of every screen.
python3 - "$OUT" <<'PY' || echo "(Pillow missing: screenshots left as .ppm)"
import pathlib, sys
from PIL import Image, ImageDraw
out = pathlib.Path(sys.argv[1])
shots = sorted(out.glob("*.ppm"))
for p in shots:
    Image.open(p).save(p.with_suffix(".png")); p.unlink()
pngs = sorted(out.glob("[0-9]*.png"))
cols, pad, lab = 5, 12, 18
rows = (len(pngs) + cols - 1) // cols
sheet = Image.new("RGB", (cols * (240 + pad) + pad, rows * (240 + pad + lab) + pad), (24, 24, 28))
d = ImageDraw.Draw(sheet)
for i, p in enumerate(pngs):
    x = pad + (i % cols) * (240 + pad); y = pad + (i // cols) * (240 + pad + lab)
    sheet.paste(Image.open(p), (x, y))
    d.text((x, y + 242), p.stem[3:].replace("_", " "), fill=(180, 186, 196))
sheet.save(out / "screens.png")
print(f"{len(pngs)} screenshots + screens.png in {out}")
PY
