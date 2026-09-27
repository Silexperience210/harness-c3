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

## Tap a card = open the agent's terminal

A tap on a card is `agent.open`, and the daemon does not act on it itself: it
forwards it to the connected app as `{"type":"dial_open","payload":{agentId,
reason?}}` (on macOS the app opens a tile). The bridge answers it:

- the agent's tmux session has **no client** → it opens a terminal running
  `tmux attach-session -t <session>`;
- a client is **already attached** → it selects the agent's pane and, if
  `wmctrl` is installed (X11), raises that window;
- `reason: "question"` (a question screen that came up on its own) only
  brings forward what is already on screen: no window per question.

Terminal: the first found of `x-terminal-emulator` (Debian/Ubuntu default),
`gnome-terminal`, `konsole`, `kitty`, `alacritty`, `wezterm`, `foot`,
`ghostty`, `xfce4-terminal`, `xterm`. Choose with an override in the unit:

```sh
systemctl --user edit harness-desk
# [Service]
# Environment=HARNESS_DIAL_TERMINAL=kitty
# or any command, {session} is replaced (no shell involved):
# Environment="HARNESS_DIAL_OPEN=wezterm start -- tmux attach -t {session}"
```

**Nothing opens?** `journalctl --user -u harness-desk -f`, tap a card:

- `no DISPLAY/WAYLAND_DISPLAY` → the user service cannot reach your desktop:
  `systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XAUTHORITY`
  then `systemctl --user restart harness-desk` (most desktops do this at
  login; add it to your session autostart if yours does not);
- `no terminal found` → set `HARNESS_DIAL_TERMINAL` / `HARNESS_DIAL_OPEN`;
- `no tmux pane known` → the agent was not started in tmux (use `hermes-dial`);
- no `open …` line at all → the bridge running is an older copy: reinstall
  `harness-desk-linux.mjs` to `~/.harness/` and restart the unit.

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

## A card that says "inactif"

The dial draws a card's state from the daemon's **turn events**, never from the
desk content: a session the daemon has just discovered reads `inactif` until it
actually works — accurate, not a fault, and nothing to fix on the dial side.
`/api/status` shows why: `sessions[].sessionId` is empty until the engine's own
hook binds the session.

The daemon installs that hook itself. For Hermes it writes a pair into
`~/.hermes/config.yaml` (`on_session_start` and `pre_llm_call` →
`~/.harness/cli/notify.mjs --engine hermes`, inside the `# machine-adapter`
block), so a Hermes agent started in a `harness-*` pane *after* that install is
followed automatically. A working turn then looks like this:

```
[hooks] pre_llm_call · engine=hermes
[turn] 20260927 started · engine=hermes · bytes=23
[turn] 20260927 ended · 2000ms
[recap] done · recap="Bonjour" · device=true      ← pushed to the dial
```

**Do not bridge this yourself.** The daemon only trusts a hook whose caller is a
descendant of the engine process it resolved in the pane, and refuses everything
else (`unmatched hermes hook … caller is not a descendant of that engine
process`, then `no_matching_engine_process`, then `UNBOUND_HOOK` on a turn): an
external `curl`, even with the right `x-harness-hook-token`
(`~/.harness/cli/data/hook-credential`, mode 600), never lands. Only a real hook
— a child of the agent process — counts, and Hermes already has one.

Two limits worth knowing:

- a session driven through the **gateway** (Telegram, no tmux pane) is not
  watched at all — the daemon observes panes only, so it can never appear as a
  card; and
- the **first** turn of a session loses its `turn_started` ("the turn opened
  before the session was attached") and therefore produces no recap. Later
  turns are complete.

```sh
journalctl --user -u harness.service | grep -E '\[turn\]|\[recap\]|\[hooks\]'
curl -s -H 'x-adapter-local: 1' http://127.0.0.1:18473/api/status   # sessions[].sessionId
```
