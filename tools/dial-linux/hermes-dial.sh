#!/bin/sh
# Launch the Hermes CLI under the name the Harness daemon matches.
#
# Why not just run `hermes`? The daemon identifies the engine by PROCESS NAME
# ("hermes" / "hermes-agent"). ~/.local/bin/hermes is a shell script that ends
# up running "python3", so the dial never sees it. Executed through a symlink
# named ~/.harness/bin/hermes-agent, the process carries the expected name.
set -e
BIN="$HOME/.harness/bin"
mkdir -p "$BIN"
PY=$(command -v python3 || echo /usr/bin/python3)
ln -sf "$PY" "$BIN/hermes-agent"
exec "$BIN/hermes-agent" -I "$(dirname "$0")/dial-entry.py" "$@"
