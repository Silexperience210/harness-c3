#!/bin/sh
# Run the desk bridge with the Node runtime shipped by the Harness CLI (>= 22:
# it needs global WebSocket + fetch). The managed path contains the version, so
# it changes on every CLI update — hence the glob, then the system node.
set -e
NODE=""
for candidate in "$HOME"/.harness/runtime/node-v*-linux-x64/bin/node; do
    [ -x "$candidate" ] && NODE="$candidate"
done
[ -n "$NODE" ] || NODE=$(command -v node || true)
[ -n "$NODE" ] || { echo "no usable node runtime found" >&2; exit 1; }
exec "$NODE" "$HOME/.harness/harness-desk-linux.mjs"
