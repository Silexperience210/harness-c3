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
// dial's gestures (focus / agent.open / turn.stop) travel back to those agents.
// Nothing leaves the machine.
//
// Usage: node harness-desk-linux.mjs
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
    if (text.includes("dial_status")) console.log(new Date().toISOString(), "dial:", text.slice(0, 160));
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
    const ids = (state.sessions || [])
      .filter((s) => s.engine && s.engine !== "terminal")
      .map((s) => s.id);
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
