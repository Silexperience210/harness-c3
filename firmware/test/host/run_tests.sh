#!/usr/bin/env bash
# Host tests for the cable protocol implementation — no ESP-IDF required.
# Builds cable_frame.c and cable_client.c with plain gcc and the vendored
# cJSON, runs both suites, exits non-zero on any failure.
set -euo pipefail

cd "$(dirname "$0")"
MAIN="../../main"
BUILD="$(mktemp -d)"
trap 'rm -rf "$BUILD"' EXIT

CFLAGS="-std=c11 -Wall -Wextra -Werror -g -I$MAIN -Icjson"

echo "== build test_cable_frame =="
gcc $CFLAGS test_cable_frame.c "$MAIN/cable_frame.c" -o "$BUILD/test_cable_frame"

echo "== build test_messages =="
gcc $CFLAGS -DCABLE_HOST_TEST test_messages.c layout_tu.c "$MAIN/cable_frame.c" \
    "$MAIN/cable_client.c" cjson/cJSON.c -o "$BUILD/test_messages"

echo "== run test_cable_frame =="
"$BUILD/test_cable_frame" ../vectors/cable_frame.txt

echo "== run test_messages =="
"$BUILD/test_messages"

echo "== vectors regeneration check =="
python3 ../../scripts/gen_cable_vectors.py --check

echo "ALL HOST TESTS PASSED"
