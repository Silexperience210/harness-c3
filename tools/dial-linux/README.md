# Local desk for the C3 dial on Linux

The dial only ever shows the agents that belong to the **desk published by a
connected app**. From the daemon's own code:

```js
async listAgents () {
  const all = await this.listAgentsFlat();          // every known agent
  const byId = new Map(all.map(a => [a.id, a]));
  const onDesk = this.swarms === null ? []          // no app connected
               : this.desk.map(id => byId.get(id)).filter(Boolean);
  …
}
```

With no app connected the daemon logs `cable: tab (no window)` and the dial
keeps showing "no agent", however many agents are alive on the machine. The
Harness **desktop app is macOS-only** (the download page offers macOS only), so
on Linux nothing publishes that desk and the dial looks dead.

This folder publishes it locally — over loopback, no cloud, no extra service:

```
[C3 dial] ──USB──> harness daemon ──ws://127.0.0.1:18473/api/local-ws──> harness-desk-linux.mjs
                        ▲                                                        │
                        └────────── GET /api/status (x-adapter-local: 1) ────────┘
```

## How the bridge talks to the daemon

1. `GET http://127.0.0.1:18473/api/status` with the header `x-adapter-local: 1`
   → `machineId` and `sessions[]` (`id`, `engine`, `tmuxPane`, `cwd`).
2. WebSocket `ws://127.0.0.1:18473/api/local-ws`, then:

```jsonc
{"type":"machine_select","payload":{"machineId":"<id>","localProtocolVersion":1}}
{"type":"app_panes", "payload":{"agentIds":["<agentId>"],"foreground":true}}
{"type":"app_swarms","payload":{"active":"linux",
  "swarms":[{"id":"linux","name":"Hermes","agentIds":["<agentId>"],"panes":1}]}}
```

Both frames are required. `app_panes` alone gets you
`[cable] window tiles: <id>` but leaves `swarms` null, so `listAgents()`
returns `[]` and the daemon keeps sending `cable: agents → 0 of N`: an empty
dial. With `app_swarms` you get `cable: tab 817a · 1 in all` then
`cable: agents → 1 of 1`, and the device itself confirms
`cable_client: agents: 1 of 1`.

The daemon does the rest: it pushes the cards to the dial, and the dial's
gestures (`focus`, `agent.open`, `turn.stop`) travel back to those agents.

## Files

| file | role |
| --- | --- |
| `harness-desk-linux.mjs` | the bridge: polls `/api/status`, publishes `app_panes` + `app_swarms` |
| `harness-desk-linux.sh` | runs the bridge with the CLI's managed Node runtime (its path carries the version) |
| `harness-desk.service` | user unit, `After=harness.service`, `Restart=always` |
| `harness.service` | user unit for the daemon itself (`harness start -f`) |
| `hermes-dial` | starts a Hermes agent in a `harness-*` tmux session so it appears as a card |
| `hermes-dial.sh` + `dial-entry.py` | start the Hermes CLI under the process name the daemon matches |

## Install

```sh
install -Dm755 harness-desk-linux.mjs ~/.harness/harness-desk-linux.mjs
install -Dm755 harness-desk-linux.sh  ~/.harness/harness-desk-linux.sh
install -Dm755 hermes-dial            ~/.local/bin/hermes-dial
install -Dm755 hermes-dial.sh         ~/.harness/bin/hermes-dial.sh
install -Dm644 dial-entry.py          ~/.harness/bin/dial-entry.py
install -Dm644 harness-desk.service   ~/.config/systemd/user/harness-desk.service
install -Dm644 harness.service        ~/.config/systemd/user/harness.service
systemctl --user daemon-reload
systemctl --user enable --now harness.service harness-desk.service
loginctl enable-linger "$USER"        # both survive logout and reboot
```

Needs: the Harness CLI signed in (`harness login`), Node ≥ 22 (global
`WebSocket`/`fetch` — the runtime the CLI ships is fine), tmux.

## Verify

```sh
harness status                      # ● running · backend connected · agents N available
journalctl --user -u harness.service -f | grep cable
#   [cable] window tiles: 817af2d9 ddd4718b
#   [cable] cable: tab 817a ddd4 · 2 in all
#   [cable] cable: agents → 2 of 2
tail -2 ~/.harness/logs/dial-$(date +%Y%m%d).log
#   I (…) cable_client: agents: 2 of 2      ← what the DEVICE sees: the only proof that counts
```

Start agents with `hermes-dial` (one per call, each in its own tmux session),
then on the dial: swipe ←/→ between cards, tap a card to open it.

## Pitfalls

- `app_panes` without `app_swarms` = `agents → 0 of N` and an empty dial.
- The daemon drops a silent local client after ~40 s, and a disconnect clears the
  desk (`[cable] window tiles: (none)`) — hence the ≤ 15 s refresh and
  `Restart=always`.
- A tmux session not named `harness-*` is ignored; a CLI running as `python3` is
  not recognised as an engine (hence the `hermes-agent` symlink trick).
- `harness new hermes` fails with `INVALID_PERMISSION_MODE` — the CLI sends
  `permissionMode: "auto"` while Hermes declares no permission mode
  (autonomous-ai/openharness#371).
- The card label comes from the daemon (`scratch · 817a` = cwd + pane id), not
  from the tmux session name.
- Backlight 16 / `screen=2` in the log = dimmed after 60 s (off after 10 min):
  firmware behaviour, not a fault.
