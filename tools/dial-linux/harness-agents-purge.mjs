
// Supprime tous les agents SAUF celui passe en argument (id prefixe accepte).
import { randomUUID } from "node:crypto";
const keep = process.argv[2];
const HTTP = "http://127.0.0.1:18473";
const st = await (await fetch(HTTP + "/api/status", { headers: { "x-adapter-local": "1" } })).json();
const cibles = st.sessions.filter((s) => !s.id.startsWith(keep));
console.log("a supprimer:", cibles.length, "| conserve:", st.sessions.length - cibles.length);
for (const c of cibles) {
  const ws = new WebSocket("ws://127.0.0.1:18473/api/local-ws");
  await new Promise((res) => {
    ws.onopen = () => {
      ws.send(JSON.stringify({ type: "machine_select", payload: { machineId: st.machineId, localProtocolVersion: 1 } }));
      setTimeout(() => ws.send(JSON.stringify({
        type: "agent_delete", requestId: randomUUID(),
        payload: { agentId: c.id, expectedSessionId: c.sessionId || null },
      })), 300);
    };
    ws.onmessage = (ev) => {
      const s = String(ev.data);
      if (/agent_delete_result|agent_deleted|error/i.test(s)) { console.log(" ", c.name, "->", s.slice(0, 120)); res(); }
    };
    setTimeout(res, 3500);
  });
  ws.close();
}
