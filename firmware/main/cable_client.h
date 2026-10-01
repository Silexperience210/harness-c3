// Message layer of the Harness "cable" protocol: what the dial and the daemon
// SAY to each other, on top of the frames cable_link moves. The normative
// description is PROTOCOL.md at the repository root (extracted from
// autonomous-ai/openharness, MIT) — that document wins any disagreement.
//
// Two things this layer owns and nothing else does:
//
//   THE HANDSHAKE. The dial says `hello`, the daemon answers `welcome`, the
//   dial asks for `agents.list`. Until `welcome` lands there is no session
//   and the screen says "Not connected".
//
//   BEING LENIENT. Everything arriving here came off a cable that carries
//   bootloader chatter at every boot and may be talking to a daemon newer
//   than this firmware. A message that cannot be read is discarded and
//   COUNTED — never a reason to drop the link, and never a reason to reboot.
//
// DELIBERATELY HOST-COMPILABLE. Compiled with -DCABLE_HOST_TEST this file's
// translation unit needs only cJSON and a handful of stubs (see
// test/host/test_messages.c), which is how the golden message tests run on a
// laptop instead of on the device through a flash cycle.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

// ── protocol constants ──────────────────────────────────────────────────────

// Which product this dial belongs to, stated in every greeting. The framing
// magic (cable_frame.h) is the first lock; this is the second: the daemon's
// rule is a POSITIVE match — a greeting that does not name this product is
// foreign, and absence has to mean no.
#define CABLE_PRODUCT "harness"

// The MESSAGE-vocabulary version. Separate from CABLE_FRAME_VERSION on
// purpose: adding a message is a change here, changing the envelope is a
// change there. 3: + question.close.
#define CABLE_PROTO_VERSION 3

// Hardware self-description reported in `hello.hw`. Informational — a daemon
// that predates the field ignores it. Distinct from the reference boards'
// "cst9217+axp2101"/"cst816s" so a log line names what this dial actually is.
// This is the BASE name: app_main appends the touch controller it detected at
// boot ("+cst816d", …), the way upstream self-describes its variants.
#define CABLE_HW_NAME "esp32c3+gc9a01"

// Field caps (PROTOCOL.md §11).
#define ID_MAX   48   // agentId / request id (uuid or 32-hex) + NUL
// NOT "NAME_MAX": that is a POSIX name <limits.h> defines as 255 on newlib.
// With an #ifndef guard the value depended on include order, so one
// translation unit laid cable_agent_t out with 40-byte names and another with
// 255-byte ones — the UI read every agent after the first (and every copied
// question) at the wrong offsets. A private name cannot collide.
#define CABLE_NAME_MAX 40

// How many agents the store holds. The list is the window's ACTIVE TAB and
// nothing else; the rest of the fleet arrives as agents.end.total.
#ifdef CONFIG_HARNESS_MAX_AGENTS
#define CABLE_MAX_AGENTS CONFIG_HARNESS_MAX_AGENTS
#else
#define CABLE_MAX_AGENTS 8
#endif

// Session timing (PROTOCOL.md §3.1). Over a cable there is no connection to
// lose: the daemon quitting and the daemon having nothing to say are both
// silence, so liveness is entirely cadence-based.
#define CABLE_HELLO_ALONE_MS   2000   // no session: greet every 2 s
#define CABLE_HELLO_SESSION_MS 15000  // session up: keepalive + re-introduction
#define CABLE_SILENCE_MS       15000  // nothing of ANY kind → session down

// One inbound JSON message. A `question` with several options is the largest
// thing the vocabulary legitimately sends.
#define CABLE_JSON_MAX 2048

// Question screen limits (PROTOCOL.md §5.19, §11).
#define CABLE_Q_MAX        4    // questions per screen
#define CABLE_OPT_MAX      6    // options per question
#define CABLE_OPT_TEXT_MAX 80   // per option label
#define CABLE_Q_KEY_MAX    96   // answer-map key, echoed back untouched
#define CABLE_Q_TEXT_MAX   160  // prompt text

// Questions held at once. Two agents can stop to ask at the same moment; the
// dial answers them one after the other instead of letting the second
// overwrite the first (which then pulsed forever with nothing to answer).
#define CABLE_Q_PENDING_MAX 3

// ── stored agent row ────────────────────────────────────────────────────────
// The wire does not carry an agent "status" enum: `state` is DERIVED from the
// message kinds (PROTOCOL.md §6) and is one of:
//   "idle" | "running" | "waiting" | "done" | "error"
typedef struct {
    char id[ID_MAX];
    char name[CABLE_NAME_MAX];
    char engine[12];         // claude|codex|cursor|opencode|… ; "" when unsaid
    char state[12];          // derived tile state, see above
    char summary[100];       // last status line (turn.started) or recap (summary)
    char machine_id[ID_MAX]; // which machine this agent lives on ("" if unsaid)
    char machine[CABLE_NAME_MAX];  // that machine's display name
} cable_agent_t;

// One row of the window's unread list, as `notif.replace` carries it.
typedef struct {
    char agent_id[ID_MAX];
    char name[CABLE_NAME_MAX];
    char machine[CABLE_NAME_MAX];
    char summary[100];
    bool question;           // blocked on an answer vs finished turn
} cable_notif_t;

#define CABLE_NOTIF_MAX 8

// One question of a `question` message, then the whole request.
typedef struct {
    char key[CABLE_Q_KEY_MAX];                     // answer-map key, echoed verbatim
    char q[CABLE_Q_TEXT_MAX];                      // prompt text
    char options[CABLE_OPT_MAX][CABLE_OPT_TEXT_MAX];
    int  opt_count;
    bool multi;                                    // multi-select: labels joined ", "
} cable_question_item_t;

typedef struct {
    char agent_id[ID_MAX];
    char request_id[ID_MAX];  // the daemon's opaque id, echoed back as requestId
    char name[CABLE_NAME_MAX];      // who is asking (rides on the frame)
    char machine[CABLE_NAME_MAX];
    int  count;
    cable_question_item_t items[CABLE_Q_MAX];
} cable_question_t;

// ── UI event sink ───────────────────────────────────────────────────────────
// All callbacks fire ON THE LINK READER TASK (or from cable_client_poll), so
// they must never touch LVGL directly — post to the UI task and return.
typedef struct {
    // Session came up (`welcome`) or went down (silence / host gone).
    void (*session)(bool up, const char *machine_name, void *ctx);
    // The staged agent list changed (agents.end applied, or cleared on
    // session down). Re-read it with cable_client_list_agents().
    void (*agents_changed)(void *ctx);
    // An agent's derived state moved (turn.started/done/error, summary).
    // `text` is the status line or recap; beep/notify policy is the UI's.
    // `started`: this moved a listed agent INTO "running" from rest — a turn
    // began. A status-line update on a running turn, the "waiting for your
    // answer" line of a question and an id the list does not hold are not.
    void (*agent_event)(const char *agent_id, const char *state, const char *text,
                        bool notify, bool beep, bool started, void *ctx);
    // The unread list changed: replayed whole (notif.replace), one row dropped
    // (notif.seen), or a question row cleared because it was answered/closed.
    // Always the complete current list. Max CABLE_NOTIF_MAX rows.
    void (*notif)(const cable_notif_t *items, int count, void *ctx);
    // An agent stopped to ask. The question is already queued; the client has
    // already sent agent.open(reason="question"). `q` points into the
    // client's store and is only valid during the call — the UI copies what
    // it needs with cable_client_copy_question().
    void (*question)(const cable_question_t *q, void *ctx);
    // A pending question was dealt with elsewhere (question.close with a
    // matching id), or the session ended. The UI re-reads the queue.
    void (*question_closed)(void *ctx);
    // Transient text (toast / turn.error message).
    void (*toast)(const char *text, void *ctx);
    // The daemon's surface moved; centre that agent's tile if we hold it.
    void (*focus)(const char *agent_id, void *ctx);
    void *ctx;
} cable_client_ui_t;

void cable_client_set_ui(const cable_client_ui_t *ui);

// ── identity (injected by the platform, never a constant here) ─────────────
// `fw` is the RUNNING image's version (esp_app_get_description()->version on
// device) suffixed "-c3" by the caller; the daemon's offer decision compares
// this string, and two sources cannot be kept equal. `mac` is the WiFi STA
// MAC, uppercase, colon-separated — also the device's USB serial number.
void cable_client_set_identity(const char *fw, const char *hw, const char *mac);

// Monotonic milliseconds, supplied by the platform: esp_timer_get_time()/1000
// on device, a fake clock in host tests.
uint32_t cable_platform_millis(void);

// ── lifecycle ───────────────────────────────────────────────────────────────

// Zero all state and counters. Safe to call at boot before any other call.
void cable_client_init(void);

// Device only: allocate the store lock. Call once, after cable_client_init()
// and BEFORE cable_link_start(): frames can arrive the moment the link runs.
// Returns false when the lock could not be allocated.
bool cable_client_start(void);

// Drive the session machine once (hello cadence + silence watchdog,
// PROTOCOL.md §3.1). On the device it runs ON THE LINK READER TASK — handed
// to cable_link_start() as its tick — so the session machine and the frame
// handlers never race each other; host tests call it with a fake clock.
void cable_client_poll(void);

// Frame entry point — matches cable_frame_cb so it can be handed straight to
// cable_link_start(), and called directly by host tests.
void cable_client_handle_frame(uint8_t version, uint8_t type,
                               const uint8_t *payload, size_t payload_len,
                               void *ctx);

// ── session state ───────────────────────────────────────────────────────────
bool        cable_client_is_connected(void);
const char *cable_client_machine_name(void);   // "" until welcome
const char *cable_client_machine_id(void);     // "" until welcome

// Copy the current agent list into `out`; returns how many were written.
int  cable_client_list_agents(cable_agent_t *out, int max);
// Account-wide agent count from agents.end.total (the fleet badge number).
int  cable_client_agent_total(void);
// Whether a window is open at the far end (agents.end.tab non-empty).
bool cable_client_has_window(void);

// The oldest pending question, or NULL. NOT thread-safe: the store belongs
// to the link task; host tests use it, the UI uses the copy below.
const cable_question_t *cable_client_pending_question(void);

// Copy the oldest pending question into `out` under the store lock. Returns
// false (and leaves `out` alone) when nothing is pending.
bool cable_client_copy_question(cable_question_t *out);

// How many questions are waiting (0..CABLE_Q_PENDING_MAX).
int  cable_client_question_count(void);

// Copy the request id of the oldest pending question into `out` ("" when
// none). Lets the UI check "is mine still first?" without copying 3 KB.
void cable_client_question_head_id(char *out, size_t cap);

// How many rows of the unread list are questions (the ring's amber pulse).
int  cable_client_notif_question_count(void);

// Copy the current unread list; returns how many rows were written.
int  cable_client_list_notifs(cable_notif_t *out, int max);

// Message-layer health. `bad` = unreadable JSON or no `t`; `unknown` =
// well-formed but not in this build's vocabulary (a daemon running ahead,
// never a fault). Neither ever drops the link.
void cable_client_counters(uint32_t *bad, uint32_t *unknown);

// ── what the dial says (v1 vocabulary, PROTOCOL.md §4) ─────────────────────
// Construction lives ONLY here. v1 deliberately has no builders for voice,
// machine wheel, swarms, scroll or models — and never answers fw.offer
// (PROTOCOL.md §7 is ESP32-S3-only; see SPEC.md §9).

// Ask for the agent list (streamed back as agents.begin/agent×N/agents.end).
void cable_client_request_agents(void);

// The tile the carousel settled on — where the user is LOOKING.
void cable_client_send_focus(const char *agent_id);

// "Put this one in front of me". `reason` NULL/"" for a person's tap, the
// only defined value is "question" (absent reads the same to an old daemon).
void cable_client_send_open(const char *agent_id, const char *reason);

// Answer a `question`. `request_id` is echoed byte-for-byte; `answers` is the
// UI's own object, keyed by the exact keys the daemon asked with, travelling
// verbatim. Drops the matching question from the queue, clears that agent's
// question row from the unread list, and moves its tile back to "running"
// (the turn that stopped to ask carries on).
void cable_client_answer(const char *agent_id, const char *request_id,
                         const cJSON *answers);

// Interrupt that agent's running turn, and only that one (PROTOCOL.md §4.9).
void cable_client_send_stop(const char *agent_id);

// Touch scrollpad (PROTOCOL.md §4.13): the dial reports finger MOVEMENT for
// the computer's window. `phase` is "down" | "move" | "up"; `dy` is device
// pixels since the last report (positive = down the glass, 0 is never
// elided); `v` (px/s, signed like dy) is sent only when `with_v` — i.e. on
// "up", where it becomes the fling.
void cable_client_send_scroll(const char *phase, int dy, int v, bool with_v);

// ── utilities (exposed for host tests) ──────────────────────────────────────

// snprintf-style copy that never leaves half a UTF-8 sequence at the cut:
// a truncated "Café" must not end in a lone 0xC3 (LVGL draws garbage, and
// cJSON would re-emit invalid UTF-8 to the daemon). Returns strlen(dst).
size_t cable_utf8_copy(char *dst, size_t cap, const char *src);
