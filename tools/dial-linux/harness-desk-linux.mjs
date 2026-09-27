// harness-desk-linux — publish a local "desk" for the Harness C3 dial.
//
// Why this exists: the daemon only sends the dial the agents that belong to the
// desk published by a CONNECTED APP — listAgents(): swarms === null ? [] :
// desk.map(...). With no app connected it logs `cable: tab (no window)` and the
// dial keeps showing "no agent", however many agents are running locally. The
// Harness desktop app is macOS-only, so on Linux nothing publishes that desk.
//
// This script plays that role, on loopback only:
//   GET /api/status     header x-adapter-local: 1   -> machineId + sessions[]
//   WS  /api/local-ws   machine_select, then:
//     app_panes  {agentIds:[...], foreground:true}                -> window tiles
//     app_swarms {active, swarms:[{id,name,agentIds,panes}]}      -> tab(s)
// Both frames are required: app_panes alone leaves `swarms` null, so
// listAgents() returns [] and the daemon keeps sending `agents → 0 of N`.
//
// The daemon does the rest: it pushes the agent cards to the dial, and the
// dial's gestures travel back. Most of them (turn.stop, answers) the daemon
// carries out itself; but a TAP on a card (`agent.open`) is a request to the
// WINDOW: the daemon forwards it to the local app as
//   {"type":"dial_open","payload":{"machineId","agentId","reason"?}}
// On macOS the app opens a tile. Here we open a terminal attached to the
// agent's tmux session (or bring it forward if one is already attached).
// Nothing leaves the machine.
//
// Usage: node harness-desk-linux.mjs
//   HARNESS_DIAL_TERMINAL=kitty          pick the terminal (default: first found)
//   HARNESS_DIAL_OPEN="wezterm start -- tmux attach -t {session}"   full command
import { execFile, spawn } from "node:child_process";

const WS_URL = "ws://127.0.0.1:18473/api/local-ws";
const HTTP = "http://127.0.0.1:18473";
const POLL_MS = 8_000;
const REFRESH_MS = 15_000; // the daemon drops a silent local client after ~40 s
const SWARM_NAME = "Hermes"; // label of the tab shown on the dial

let machineId = "";
let ws = null;
let agentIds = [];
let sentAt = 0;
let wsStartedAt = 0;   // when the current socket attempt began
let panes = new Map(); // agentId -> tmux pane (e.g. "%12"), from /api/status

const log = (...a) => console.log(new Date().toISOString(), ...a);

// ── dial_open: a tap on a card ──────────────────────────────────────────────
// Every external command runs through execFile/spawn with an argument list,
// never a shell: nothing from the socket can become a command line.
function run(cmd, args) {
  return new Promise((resolve) => {
    execFile(cmd, args, { timeout: 4000 }, (err, stdout) => resolve(err ? null : String(stdout).trim()));
  });
}

async function has(cmd) {
  return (await run("sh", ["-c", 'command -v "$1"', "sh", cmd])) !== null;
}

// How each terminal is told "run this command, with this window title".
const TERMINALS = {
  // Debian/Ubuntu's configured default; policy requires `-T title` and `-e cmd…`.
  "x-terminal-emulator": (t, c) => ["-T", t, "-e", ...c],
  "gnome-terminal": (t, c) => ["--title", t, "--", ...c],
  konsole: (_t, c) => ["-e", ...c],
  kitty: (t, c) => ["--title", t, ...c],
  alacritty: (t, c) => ["--title", t, "-e", ...c],
  wezterm: (_t, c) => ["start", "--", ...c],
  foot: (t, c) => ["--title", t, ...c],
  ghostty: (t, c) => [`--title=${t}`, "-e", ...c],
  "xfce4-terminal": (t, c) => ["--title", t, "-x", ...c],
  xterm: (t, c) => ["-T", t, "-e", ...c],
};

async function terminalCommand(session) {
  const attach = ["tmux", "attach-session", "-t", session];
  const custom = process.env.HARNESS_DIAL_OPEN;
  if (custom) {
    const words = custom.split(/\s+/).filter(Boolean).map((w) => w.replaceAll("{session}", session));
    return [words[0], words.slice(1)];
  }
  const wanted = process.env.HARNESS_DIAL_TERMINAL;
  const order = wanted ? [wanted] : Object.keys(TERMINALS);
  for (const name of order) {
    if (!(await has(name))) continue;
    const build = TERMINALS[name] || TERMINALS["x-terminal-emulator"];
    return [name, build(session, attach)];
  }
  return null;
}

const lastOpen = new Map(); // session -> ms: a double tap must not open two windows

async function openAgent(agentId, reason) {
  const pane = panes.get(agentId);
  if (!pane) return log("open", String(agentId).slice(0, 8), "· no tmux pane known for this agent");
  const session = await run("tmux", ["display-message", "-p", "-t", pane, "#{session_name}"]);
  if (!session || !/^[\w.:@+-]{1,120}$/.test(session)) {
    return log("open", agentId.slice(0, 8), "· tmux pane", pane, "not found");
  }
  // Show the agent's own pane in its session, whoever is looking at it.
  await run("tmux", ["select-window", "-t", pane]);
  await run("tmux", ["select-pane", "-t", pane]);
  const clients = await run("tmux", ["list-clients", "-t", session, "-F", "#{client_tty}"]);
  if (clients) {
    // Already on screen: bring that window forward if the desktop lets us.
    if (await has("wmctrl")) await run("wmctrl", ["-a", session]);
    return log("open", session, "· already attached, brought forward");
  }
  // A question screen coming up on its own only brings forward what is
  // already on screen (PROTOCOL 4.11): it must not pop a window per question.
  if (reason === "question") return log("open", session, "· question, not on screen: left alone");
  const now = Date.now();
  if (now - (lastOpen.get(session) || 0) < 3000) return;
  lastOpen.set(session, now);
  if (!process.env.DISPLAY && !process.env.WAYLAND_DISPLAY) {
    return log("open", session, "· no DISPLAY/WAYLAND_DISPLAY in this service's environment:",
      "run `systemctl --user import-environment DISPLAY WAYLAND_DISPLAY XAUTHORITY`",
      "then `systemctl --user restart harness-desk`");
  }
  const cmd = await terminalCommand(session);
  if (!cmd) {
    return log("open", session, "· no terminal found: set HARNESS_DIAL_TERMINAL (kitty, gnome-terminal, …)",
      "or HARNESS_DIAL_OPEN");
  }
  const [bin, args] = cmd;
  try {
    const child = spawn(bin, args, { detached: true, stdio: "ignore" });
    child.on("error", (err) => log("open", session, "·", bin, "failed:", err.message));
    child.unref();
    log("open", session, "· opened in", bin);
  } catch (err) {
    log("open", session, "·", bin, "failed:", err.message);
  }
}

async function status() {
  const res = await fetch(HTTP + "/api/status", { headers: { "x-adapter-local": "1" } });
  if (!res.ok) throw new Error("status HTTP " + res.status);
  return res.json();
}

function send(frame) {
  try {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(frame));
  } catch (err) {
    /* the socket is closing: the reconnect path re-publishes the desk */
  }
}

function publish(force = false) {
  const now = Date.now();
  if (!force && now - sentAt < REFRESH_MS) return;
  sentAt = now;
  send({ type: "app_panes", payload: { agentIds, foreground: true } });
  send({
    type: "app_swarms",
    payload: {
      active: "linux",
      swarms: [{ id: "linux", name: SWARM_NAME, agentIds, panes: agentIds.length }],
    },
  });
}

function connect() {
  ws = new WebSocket(WS_URL);
  wsStartedAt = Date.now();
  ws.onopen = () => {
    console.log(new Date().toISOString(), "local bridge connected");
    send({ type: "machine_select", payload: { machineId, localProtocolVersion: 1 } });
    publish(true); // the desk is cleared when a client disconnects: re-assert it
  };
  ws.onmessage = (event) => {
    const text = String(event.data);
    let frame;
    try {
      frame = JSON.parse(text);
    } catch {
      return;
    }
    if (frame.type === "dial_status") log("dial:", text.slice(0, 160));
    else if (frame.type === "dial_open" && typeof frame.payload?.agentId === "string") {
      openAgent(frame.payload.agentId, frame.payload.reason).catch((err) => log("open failed:", err.message));
    }
  };
  ws.onclose = (event) => {
    console.log(new Date().toISOString(), "bridge closed (" + event.code + ") · retry in 3 s");
    setTimeout(connect, 3000);
  };
  ws.onerror = () => {};
}

// A daemon restart can leave the socket half-open — no open, no close event —
// and then the bridge silently stops publishing: the desk is cleared and the
// dial drops back to "no agent". So never trust the socket: if it is not OPEN
// and the attempt is older than 10 s, force a fresh connection.
function ensureSocket() {
  if (ws && ws.readyState === WebSocket.OPEN) return;
  if (ws && Date.now() - wsStartedAt < 10000) return; // a connect is in flight
  console.log(new Date().toISOString(), "bridge not open · reconnecting");
  try {
    if (ws) ws.close();
  } catch {}
  connect();
}

async function loop() {
  ensureSocket();
  try {
    const state = await status();
    if (!machineId) machineId = state.machineId;
    const live = (state.sessions || []).filter((s) => s.engine && s.engine !== "terminal");
    const ids = live.map((s) => s.id);
    panes = new Map(live.filter((s) => s.tmuxPane).map((s) => [s.id, s.tmuxPane]));
    const changed = ids.length !== agentIds.length || ids.some((id, i) => id !== agentIds[i]);
    agentIds = ids;
    if (changed || !ws) {
      console.log(
        new Date().toISOString(),
        "desk:",
        agentIds.length ? agentIds.map((id) => id.slice(0, 8)).join(" ") : "(empty)",
      );
      publish(true);
    } else {
      publish(false);
    }
  } catch (err) {
    console.log(new Date().toISOString(), "status unavailable:", err.message);
  }
  setTimeout(loop, POLL_MS);
}

connect();
loop();
