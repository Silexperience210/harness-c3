// Golden message tests for cable_client.{c,h}: every message the device can
// EMIT is asserted byte-for-byte against PROTOCOL.md (t string + field names
// + construction order), and every daemon→device message the device MUST
// handle is fed as golden JSON and asserted against the resulting state.
//
// Built by run_tests.sh with gcc -std=c11 -Wall -Wextra -Werror -DCABLE_HOST_TEST.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cable_client.h"
#include "cable_frame.h"

static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                                            \
    do {                                                            \
        s_checks++;                                                 \
        if (!(cond)) {                                              \
            s_failures++;                                           \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
            printf(__VA_ARGS__);                                    \
            printf("\n");                                           \
        }                                                           \
    } while (0)

// ── platform stubs (the host side of cable_client's seams) ─────────────────

static uint32_t s_now_ms;
uint32_t cable_platform_millis(void) { return s_now_ms; }

typedef struct {
    uint8_t type;
    char    text[CABLE_JSON_MAX];
    size_t  len;
} sent_frame_t;

static sent_frame_t s_sent[64];
static int          s_sent_count;
static bool         s_log_framing;
static bool         s_host_present;

bool cable_link_send(uint8_t type, const uint8_t *payload, size_t payload_len)
{
    if (s_sent_count < (int)(sizeof(s_sent) / sizeof(s_sent[0]))) {
        sent_frame_t *f = &s_sent[s_sent_count++];
        f->type = type;
        f->len = payload_len < sizeof(f->text) - 1 ? payload_len : sizeof(f->text) - 1;
        memcpy(f->text, payload, f->len);
        f->text[f->len] = '\0';
    }
    return true;
}

void cable_link_set_log_framing(bool on) { s_log_framing = on; }
bool cable_link_host_present(void) { return s_host_present; }

// ── UI event capture ────────────────────────────────────────────────────────

static int  s_ev_session_up, s_ev_session_down;
static int  s_ev_agents_changed;
static int  s_ev_question, s_ev_question_closed;
static int  s_ev_toast;
static char s_ev_toast_text[160];
static int  s_ev_notif;
static int  s_ev_notif_questions;
static int  s_ev_focus;
static char s_ev_focus_id[ID_MAX];
static int  s_ev_agent_event;
static char s_ev_agent_state[16];
static int  s_ev_notify, s_ev_beep, s_ev_started;

static void ev_session(bool up, const char *machine_name, void *ctx)
{
    (void)ctx;
    if (up) { s_ev_session_up++; CHECK(strcmp(machine_name, "MacBook Pro") == 0, "session machine name '%s'", machine_name); }
    else s_ev_session_down++;
}
static void ev_agents_changed(void *ctx) { (void)ctx; s_ev_agents_changed++; }
static void ev_agent_event(const char *agent_id, const char *state, const char *text,
                           bool notify, bool beep, bool started, void *ctx)
{
    (void)agent_id; (void)text; (void)ctx;
    s_ev_agent_event++;
    snprintf(s_ev_agent_state, sizeof(s_ev_agent_state), "%s", state);
    if (notify) s_ev_notify++;
    if (beep) s_ev_beep++;
    if (started) s_ev_started++;
}
static void ev_notif(const cable_notif_t *items, int count, void *ctx)
{
    (void)ctx;
    s_ev_notif++;
    s_ev_notif_questions = 0;
    for (int i = 0; i < count; i++) if (items[i].question) s_ev_notif_questions++;
}
static void ev_question(const cable_question_t *q, void *ctx) { (void)q; (void)ctx; s_ev_question++; }
static void ev_question_closed(void *ctx) { (void)ctx; s_ev_question_closed++; }
static void ev_toast(const char *text, void *ctx)
{
    (void)ctx;
    s_ev_toast++;
    snprintf(s_ev_toast_text, sizeof(s_ev_toast_text), "%s", text ? text : "");
}
static void ev_focus(const char *agent_id, void *ctx)
{
    (void)ctx;
    s_ev_focus++;
    snprintf(s_ev_focus_id, sizeof(s_ev_focus_id), "%s", agent_id);
}

// ── fixtures ────────────────────────────────────────────────────────────────

static void feed(const char *json)
{
    cable_client_handle_frame(1, CABLE_TYPE_JSON, (const uint8_t *)json, strlen(json), NULL);
}

static void reset_all(void)
{
    s_now_ms = 0;
    s_host_present = true;
    s_log_framing = false;
    s_sent_count = 0;
    s_ev_session_up = s_ev_session_down = s_ev_agents_changed = 0;
    s_ev_question = s_ev_question_closed = s_ev_toast = s_ev_notif = 0;
    s_ev_focus = s_ev_agent_event = s_ev_notify = s_ev_beep = s_ev_started = 0;
    s_ev_agent_state[0] = '\0';
    cable_client_set_identity("0.1.0-c3", CABLE_HW_NAME, "28:84:85:90:5F:78");
    cable_client_set_ui(&(cable_client_ui_t){
        .session = ev_session,
        .agents_changed = ev_agents_changed,
        .agent_event = ev_agent_event,
        .notif = ev_notif,
        .question = ev_question,
        .question_closed = ev_question_closed,
        .toast = ev_toast,
        .focus = ev_focus,
    });
    cable_client_init();
}

static const char *last_json(void)
{
    for (int i = s_sent_count - 1; i >= 0; i--)
        if (s_sent[i].type == CABLE_TYPE_JSON) return s_sent[i].text;
    return NULL;
}

static int count_json_sent(void)
{
    int n = 0;
    for (int i = 0; i < s_sent_count; i++) if (s_sent[i].type == CABLE_TYPE_JSON) n++;
    return n;
}

// Golden frames from PROTOCOL.md.
static const char *WELCOME =
    "{\"t\":\"welcome\",\"proto\":3,\"app\":\"harness\","
    "\"machine\":{\"id\":\"mac-local\",\"name\":\"MacBook Pro\"},"
    "\"selected\":\"mac-local\",\"voiceLang\":\"en\"}";

static void start_session(void)
{
    feed(WELCOME);
    s_sent_count = 0;   // drop the agents.list that the first welcome triggers
}

// ── outbound golden tests (PROTOCOL.md §3.2, §4) ───────────────────────────

static void test_hello_exact(void)
{
    reset_all();
    cable_client_poll();   // boot greeting fires immediately
    CHECK(count_json_sent() == 1, "hello: %d frames sent", count_json_sent());
    CHECK(last_json() &&
          strcmp(last_json(),
                 "{\"t\":\"hello\",\"fw\":\"0.1.0-c3\",\"product\":\"harness\",\"proto\":3,"
                 "\"hw\":\"" CABLE_HW_NAME "\",\"mac\":\"28:84:85:90:5F:78\"}") == 0,
          "hello exact: %s", last_json() ? last_json() : "(none)");
}

static void test_hello_cadence(void)
{
    reset_all();
    cable_client_poll();                       // t=0: hello
    s_now_ms = 1999; cable_client_poll();      // too soon
    CHECK(count_json_sent() == 1, "cadence: early extra hello");
    s_now_ms = 2000; cable_client_poll();      // HELLO_ALONE_MS
    CHECK(count_json_sent() == 2, "cadence: no 2 s hello");
    start_session();                            // welcome at t=2000
    // The already-scheduled greeting still fires, then the cadence stretches
    // to HELLO_SESSION_MS measured from that send (upstream algorithm: the
    // period is chosen AT each send).
    s_now_ms = 3999; cable_client_poll();
    CHECK(count_json_sent() == 0, "cadence: early session hello");
    s_now_ms = 4000; cable_client_poll();
    CHECK(count_json_sent() == 1 && last_json() && strstr(last_json(), "\"t\":\"hello\""),
          "cadence: scheduled hello missing");
    s_now_ms = 4000 + 14999; cable_client_poll();
    CHECK(count_json_sent() == 1, "cadence: hello before 15 s session interval");
    s_now_ms = 4000 + 15000; cable_client_poll();   // HELLO_SESSION_MS
    CHECK(count_json_sent() == 2 && last_json() && strstr(last_json(), "\"t\":\"hello\""),
          "cadence: no 15 s session hello");
}

static void test_ping_pong(void)
{
    reset_all();
    start_session();
    feed("{\"t\":\"ping\"}");
    CHECK(count_json_sent() == 1 && last_json() && strcmp(last_json(), "{\"t\":\"pong\"}") == 0,
          "pong exact: %s", last_json() ? last_json() : "(none)");
}

static void test_agents_list_after_welcome(void)
{
    reset_all();
    feed(WELCOME);
    CHECK(cable_client_is_connected(), "session not up after welcome");
    CHECK(s_ev_session_up == 1, "session event count %d", s_ev_session_up);
    CHECK(strcmp(cable_client_machine_name(), "MacBook Pro") == 0, "machine name");
    CHECK(strcmp(cable_client_machine_id(), "mac-local") == 0, "machine id");
    CHECK(s_log_framing, "log framing not enabled on session up");
    CHECK(count_json_sent() == 1 && last_json() &&
          strcmp(last_json(), "{\"t\":\"agents.list\"}") == 0,
          "agents.list exact: %s", last_json() ? last_json() : "(none)");
    // Second welcome of the same session = keepalive answer: no re-ask.
    feed(WELCOME);
    CHECK(count_json_sent() == 1, "keepalive welcome re-asked (%d frames)", count_json_sent());
}

static void test_focus_and_open_exact(void)
{
    reset_all();
    start_session();
    cable_client_send_focus("a1");
    CHECK(last_json() && strcmp(last_json(), "{\"t\":\"focus\",\"agentId\":\"a1\"}") == 0,
          "focus exact: %s", last_json());
    cable_client_send_open("a2", NULL);   // a person's tap: reason ABSENT
    CHECK(last_json() && strcmp(last_json(), "{\"t\":\"agent.open\",\"agentId\":\"a2\"}") == 0,
          "agent.open (no reason) exact: %s", last_json());
    cable_client_send_open("a2", "question");
    CHECK(last_json() &&
          strcmp(last_json(), "{\"t\":\"agent.open\",\"agentId\":\"a2\",\"reason\":\"question\"}") == 0,
          "agent.open (question) exact: %s", last_json());
    // Empty id is refused, never sent.
    const int before = count_json_sent();
    cable_client_send_focus("");
    cable_client_send_open(NULL, NULL);
    CHECK(count_json_sent() == before, "empty-id messages sent");
}

static void test_answer_exact(void)
{
    reset_all();
    start_session();
    cJSON *answers = cJSON_CreateObject();
    cJSON_AddStringToObject(answers, "Which DB should I use?", "Postgres");
    cable_client_answer("a1", "q1", answers);
    cJSON_Delete(answers);
    CHECK(last_json() &&
          strcmp(last_json(),
                 "{\"t\":\"answer\",\"agentId\":\"a1\",\"requestId\":\"q1\","
                 "\"answers\":{\"Which DB should I use?\":\"Postgres\"}}") == 0,
          "answer exact: %s", last_json());
}

static void test_answer_escaping_and_verbatim_key(void)
{
    reset_all();
    start_session();
    const char *evil_key = "quote\" backslash\\ newline\n tab\t";
    const char *evil_val = "ón ✓ \"done\"\\";
    cJSON *answers = cJSON_CreateObject();
    cJSON_AddStringToObject(answers, evil_key, evil_val);
    cable_client_answer("a1", "q-9", answers);
    cJSON_Delete(answers);

    const char *sent = last_json();
    CHECK(sent != NULL, "no answer frame");
    // It must parse back as valid JSON with the key echoed BYTE-FOR-BYTE.
    cJSON *root = cJSON_Parse(sent);
    CHECK(root != NULL, "answer JSON did not parse: %s", sent);
    const cJSON *ans = root ? cJSON_GetObjectItemCaseSensitive(root, "answers") : NULL;
    const cJSON *v = ans ? cJSON_GetObjectItemCaseSensitive(ans, evil_key) : NULL;
    CHECK(cJSON_IsString(v) && strcmp(v->valuestring, evil_val) == 0,
          "answers key/value not verbatim");
    const cJSON *rid = root ? cJSON_GetObjectItemCaseSensitive(root, "requestId") : NULL;
    CHECK(cJSON_IsString(rid) && strcmp(rid->valuestring, "q-9") == 0, "requestId not echoed");
    cJSON_Delete(root);
}

// ── inbound golden tests (PROTOCOL.md §5) ──────────────────────────────────

static void feed_agent_list(void)
{
    feed("{\"t\":\"agents.begin\"}");
    feed("{\"t\":\"agent\",\"id\":\"a1\",\"name\":\"Fix login screen\",\"engine\":\"claude\","
         "\"model\":\"opus\",\"effort\":\"high\",\"machineId\":\"mac-local\",\"machine\":\"MacBook Pro\"}");
    feed("{\"t\":\"agent\",\"id\":\"a2\"}");   // bare minimum: fallbacks everywhere
    feed("{\"t\":\"agents.end\",\"total\":22,\"tab\":\"t1\"}");
}

static void test_agent_list_parse(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    CHECK(s_ev_agents_changed == 1, "agents_changed %d", s_ev_agents_changed);

    cable_agent_t agents[8];
    int n = cable_client_list_agents(agents, 8);
    CHECK(n == 2, "agent count %d", n);
    CHECK(strcmp(agents[0].id, "a1") == 0, "a1 id");
    CHECK(strcmp(agents[0].name, "Fix login screen") == 0, "a1 name");
    CHECK(strcmp(agents[0].engine, "claude") == 0, "a1 engine");
    CHECK(strcmp(agents[0].machine, "MacBook Pro") == 0, "a1 machine");
    CHECK(strcmp(agents[0].machine_id, "mac-local") == 0, "a1 machine_id");
    CHECK(strcmp(agents[0].state, "idle") == 0, "a1 initial state '%s'", agents[0].state);
    // Fallbacks: name falls back to id, machine fields to "".
    CHECK(strcmp(agents[1].name, "a2") == 0, "a2 name fallback '%s'", agents[1].name);
    CHECK(agents[1].engine[0] == '\0' && agents[1].machine[0] == '\0' &&
          agents[1].machine_id[0] == '\0', "a2 empty fallbacks");
    CHECK(cable_client_agent_total() == 22, "fleet total %d", cable_client_agent_total());
    CHECK(cable_client_has_window(), "has_window");
}

// A list refresh (agents.begin/agent/agents.end) must not repaint a running
// agent as idle, must keep its last line, must keep a questioned agent
// "waiting", and must never expose half a list while it streams.
static void test_agent_list_refresh_keeps_state(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    feed("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Compiling\"}");
    feed("{\"t\":\"question\",\"agentId\":\"a2\",\"id\":\"q9\","
         "\"questions\":[{\"key\":\"k\",\"q\":\"Ok?\",\"options\":[\"Yes\",\"No\"]}]}");

    // Mid-stream: the live list is untouched.
    feed("{\"t\":\"agents.begin\"}");
    feed("{\"t\":\"agent\",\"id\":\"a3\",\"name\":\"New one\"}");
    cable_agent_t agents[8];
    int n = cable_client_list_agents(agents, 8);
    CHECK(n == 2, "mid-stream list size %d (must still be the old list)", n);

    feed("{\"t\":\"agent\",\"id\":\"a1\",\"name\":\"Fix login screen (renamed)\"}");
    feed("{\"t\":\"agent\",\"id\":\"a2\"}");
    feed("{\"t\":\"agents.end\",\"total\":3,\"tab\":\"t1\"}");
    n = cable_client_list_agents(agents, 8);
    CHECK(n == 3, "refreshed list size %d", n);
    CHECK(strcmp(agents[0].id, "a3") == 0 && strcmp(agents[0].state, "idle") == 0,
          "new agent idle ('%s' '%s')", agents[0].id, agents[0].state);
    CHECK(strcmp(agents[1].name, "Fix login screen (renamed)") == 0, "rename applied");
    CHECK(strcmp(agents[1].state, "running") == 0, "a1 still running ('%s')", agents[1].state);
    CHECK(strcmp(agents[1].summary, "Compiling") == 0, "a1 keeps its line ('%s')", agents[1].summary);
    CHECK(strcmp(agents[2].state, "waiting") == 0, "a2 still waiting ('%s')", agents[2].state);

    // An `end` without a `begin` keeps the list.
    feed("{\"t\":\"agents.end\",\"total\":3,\"tab\":\"t1\"}");
    CHECK(cable_client_list_agents(agents, 8) == 3, "stray agents.end keeps the list");
}

static void test_agent_list_no_window(void)
{
    reset_all();
    start_session();
    feed("{\"t\":\"agents.begin\"}");
    feed("{\"t\":\"agents.end\",\"total\":0,\"tab\":\"\"}");
    CHECK(cable_client_agent_total() == 0, "total");
    CHECK(!cable_client_has_window(), "empty tab must read as no window");
}

static void test_nested_p_envelope(void)
{
    reset_all();
    start_session();
    feed("{\"t\":\"agents.begin\"}");
    feed("{\"t\":\"agent\",\"p\":{\"id\":\"a9\",\"name\":\"Nested\",\"engine\":\"codex\"}}");
    feed("{\"t\":\"agents.end\",\"p\":{\"total\":5,\"tab\":\"t7\"}}");
    cable_agent_t agents[8];
    CHECK(cable_client_list_agents(agents, 8) == 1 && strcmp(agents[0].id, "a9") == 0,
          "nested agent not parsed");
    CHECK(cable_client_agent_total() == 5 && cable_client_has_window(), "nested agents.end");
}

static void test_turn_lifecycle(void)
{
    reset_all();
    start_session();
    feed_agent_list();

    feed("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Working…\"}");
    cable_agent_t agents[8];
    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].state, "running") == 0, "state after turn.started '%s'", agents[0].state);
    CHECK(strcmp(agents[0].summary, "Working…") == 0, "status line '%s'", agents[0].summary);

    feed("{\"t\":\"turn.done\",\"agentId\":\"a1\"}");
    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].state, "done") == 0, "state after turn.done '%s'", agents[0].state);
    // A bare done rings nothing (completion is announced by the summary).
    CHECK(s_ev_beep == 0 && s_ev_notify == 0, "bare done beeped/notified");

    feed("{\"t\":\"turn.error\",\"agentId\":\"a1\",\"message\":\"boom\"}");
    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].state, "error") == 0, "state after turn.error '%s'", agents[0].state);
    CHECK(s_ev_toast == 1 && strcmp(s_ev_toast_text, "boom") == 0, "turn.error toast");
}

// `started` (the UI wakes the dial on it) is the move from rest into a turn —
// never a status-line update, a question's "waiting" line, a list refill, a
// history restore, or an agent the list does not hold.
static void test_turn_started_flag(void)
{
    reset_all();
    start_session();
    feed_agent_list();

    feed("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Working…\"}");
    CHECK(s_ev_started == 1, "idle → running is a start (%d)", s_ev_started);
    feed("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Running tests\"}");
    CHECK(s_ev_started == 1, "a status line on a running turn is not a start (%d)", s_ev_started);

    // The list refill keeps the running state and sends no agent event at all.
    const int events = s_ev_agent_event;
    feed_agent_list();
    feed("{\"t\":\"summary\",\"agentId\":\"a2\",\"recap\":\"old\",\"text\":\"t\",\"restore\":true}");
    CHECK(s_ev_started == 1 && s_ev_agent_event == events + 1, "refill + restore: no start (%d, %d events)",
          s_ev_started, s_ev_agent_event - events);

    feed("{\"t\":\"question\",\"agentId\":\"a1\",\"id\":\"q1\","
         "\"questions\":[{\"key\":\"k\",\"q\":\"Ok?\",\"options\":[\"Yes\",\"No\"]}]}");
    feed("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Waiting for your answer\"}");
    CHECK(s_ev_started == 1, "waiting → running (the question's line) is not a start (%d)", s_ev_started);

    feed("{\"t\":\"turn.started\",\"agentId\":\"zz\",\"text\":\"Working…\"}");
    CHECK(s_ev_started == 1, "an agent the list does not hold is not a start (%d)", s_ev_started);

    feed("{\"t\":\"turn.done\",\"agentId\":\"a1\"}");
    feed("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Again\"}");
    CHECK(s_ev_started == 2, "done → running is a start (%d)", s_ev_started);
    feed("{\"t\":\"turn.error\",\"agentId\":\"a2\",\"message\":\"boom\"}");
    feed("{\"t\":\"turn.started\",\"agentId\":\"a2\",\"text\":\"Retry\"}");
    CHECK(s_ev_started == 3, "error → running is a start (%d)", s_ev_started);
}

static void test_summary_variants(void)
{
    reset_all();
    start_session();
    feed_agent_list();

    // Ordinary completion: beep + notify, recap becomes the summary line.
    feed("{\"t\":\"summary\",\"agentId\":\"a1\",\"recap\":\"recap one\",\"text\":\"body one\"}");
    cable_agent_t agents[8];
    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].state, "done") == 0, "summary state");
    CHECK(strcmp(agents[0].summary, "recap one") == 0, "summary line '%s'", agents[0].summary);
    CHECK(s_ev_beep == 1 && s_ev_notify == 1, "summary beep/notify (%d/%d)", s_ev_beep, s_ev_notify);

    // quiet: beep still sounds, notification withheld.
    feed("{\"t\":\"summary\",\"agentId\":\"a1\",\"recap\":\"r\",\"text\":\"t\",\"quiet\":true}");
    CHECK(s_ev_beep == 2 && s_ev_notify == 1, "quiet summary (%d/%d)", s_ev_beep, s_ev_notify);

    // silent: neither.
    feed("{\"t\":\"summary\",\"agentId\":\"a1\",\"recap\":\"r\",\"text\":\"t\",\"silent\":true}");
    CHECK(s_ev_beep == 2 && s_ev_notify == 1, "silent summary (%d/%d)", s_ev_beep, s_ev_notify);

    // restore: history refill — no beep, no notification, no busy state.
    feed("{\"t\":\"summary\",\"agentId\":\"a1\",\"recap\":\"old\",\"text\":\"t\",\"restore\":true}");
    CHECK(s_ev_beep == 2 && s_ev_notify == 1, "restore summary (%d/%d)", s_ev_beep, s_ev_notify);
    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].summary, "old") == 0, "restore recap '%s'", agents[0].summary);
}

static void test_notif_replace(void)
{
    reset_all();
    start_session();
    feed("{\"t\":\"notif.replace\",\"items\":["
         "{\"agentId\":\"a1\",\"name\":\"Fix login screen\",\"machine\":\"MacBook Pro\","
         "\"summary\":\"…\",\"question\":false},"
         "{\"agentId\":\"a2\",\"name\":\"Second\",\"machine\":\"\",\"summary\":\"q text\","
         "\"question\":true}]}");
    CHECK(s_ev_notif == 1, "notif events %d", s_ev_notif);
    CHECK(s_ev_notif_questions == 1, "notif question rows %d", s_ev_notif_questions);
    // Rows without agentId are dropped.
    feed("{\"t\":\"notif.replace\",\"items\":[{\"name\":\"orphan\"},{\"agentId\":\"a3\"}]}");
    CHECK(s_ev_notif == 2, "notif re-replace");
}

static const char *QUESTION =
    "{\"t\":\"question\",\"agentId\":\"a1\",\"name\":\"Fix login screen\",\"engine\":\"claude\","
    "\"machine\":\"MacBook Pro\",\"id\":\"q1\",\"questions\":[{\"key\":\"Which DB should I use?\","
    "\"q\":\"Which DB should I use?\",\"options\":[\"Postgres\",\"SQLite\"],\"multi\":false}]}";

static void test_question_flow(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    s_sent_count = 0;

    feed(QUESTION);
    const cable_question_t *q = cable_client_pending_question();
    CHECK(q != NULL, "no pending question");
    if (q) {
        CHECK(strcmp(q->agent_id, "a1") == 0, "q agent");
        CHECK(strcmp(q->request_id, "q1") == 0, "q request id");
        CHECK(strcmp(q->name, "Fix login screen") == 0, "q asker name");
        CHECK(q->count == 1, "q count %d", q->count);
        CHECK(strcmp(q->items[0].key, "Which DB should I use?") == 0, "q key");
        CHECK(q->items[0].opt_count == 2 &&
              strcmp(q->items[0].options[0], "Postgres") == 0 &&
              strcmp(q->items[0].options[1], "SQLite") == 0, "q options");
        CHECK(!q->items[0].multi, "q multi");
    }
    CHECK(s_ev_question == 1, "question events %d", s_ev_question);
    // The dial asks the window to bring the agent forward, reason "question".
    CHECK(last_json() &&
          strcmp(last_json(), "{\"t\":\"agent.open\",\"agentId\":\"a1\",\"reason\":\"question\"}") == 0,
          "question agent.open: %s", last_json() ? last_json() : "(none)");

    cable_agent_t agents[8];
    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].state, "waiting") == 0, "asking agent state '%s'", agents[0].state);

    // Dedup on id: the same question arriving again is NOT re-shown.
    feed(QUESTION);
    CHECK(s_ev_question == 1, "question re-shown on duplicate id");

    // question.close with a DIFFERENT id must not close the one on screen.
    feed("{\"t\":\"question.close\",\"agentId\":\"a1\",\"id\":\"q-other\"}");
    CHECK(s_ev_question_closed == 0, "closed by stale id");
    CHECK(cable_client_pending_question() != NULL, "pending lost on stale close");

    // The matching id clears it.
    feed("{\"t\":\"question.close\",\"agentId\":\"a1\",\"id\":\"q1\"}");
    CHECK(s_ev_question_closed == 1, "question.close did not clear");
    CHECK(cable_client_pending_question() == NULL, "pending after close");

    // A fresh question, answered locally: pending clears on answer echo.
    feed(QUESTION);
    CHECK(cable_client_pending_question() != NULL, "second question not pending");
    cJSON *answers = cJSON_CreateObject();
    cJSON_AddStringToObject(answers, "Which DB should I use?", "SQLite");
    cable_client_answer("a1", "q1", answers);
    cJSON_Delete(answers);
    CHECK(cable_client_pending_question() == NULL, "pending after answer");
    CHECK(last_json() && strstr(last_json(), "\"requestId\":\"q1\"") &&
          strstr(last_json(), "\"Which DB should I use?\":\"SQLite\""),
          "answer frame: %s", last_json() ? last_json() : "(none)");
}

static void test_focus_inbound(void)
{
    reset_all();
    start_session();
    feed("{\"t\":\"focus\",\"agentId\":\"a1\"}");
    CHECK(s_ev_focus == 1 && strcmp(s_ev_focus_id, "a1") == 0, "focus event");
}

static void test_fw_offer_never_answered(void)
{
    reset_all();
    start_session();
    s_sent_count = 0;
    feed("{\"t\":\"fw.offer\",\"version\":\"9.9.9\",\"size\":3091648,"
         "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}");
    // SPEC §9 anti-brick: silence. No fw.accept, no fw.error, nothing.
    CHECK(count_json_sent() == 0, "fw.offer answered with %d frames", count_json_sent());
}

static void test_unknown_and_bad_never_fatal(void)
{
    reset_all();
    start_session();
    uint32_t bad, unknown;
    cable_client_counters(&bad, &unknown);
    CHECK(bad == 0 && unknown == 0, "counters not zero at start (%u/%u)", bad, unknown);

    feed("{\"t\":\"voice.begin\",\"agentId\":\"a1\"}");      // known upstream, out of v1 scope
    feed("{\"t\":\"machines.begin\"}");                     // machine wheel: not implemented
    feed("{\"t\":\"some.future.thing\",\"x\":1}");          // daemon running ahead
    cable_client_handle_frame(1, 0x7F, (const uint8_t *)"whatever", 8, NULL);  // unknown frame type
    cable_client_counters(&bad, &unknown);
    CHECK(unknown == 4, "unknown counter %u, want 4", unknown);

    feed("not json at all");
    feed("{\"noT\":\"here\"}");                             // no t discriminator
    cable_client_handle_frame(1, CABLE_TYPE_JSON, NULL, 0, NULL);              // empty payload
    static char big[CABLE_JSON_MAX + 1];
    memset(big, ' ', sizeof(big) - 1);
    big[0] = '{'; big[sizeof(big) - 2] = '}'; big[sizeof(big) - 1] = '\0';
    cable_client_handle_frame(1, CABLE_TYPE_JSON, (const uint8_t *)big, CABLE_JSON_MAX, NULL);
    cable_client_counters(&bad, &unknown);
    CHECK(bad == 4, "bad counter %u, want 4", bad);

    // The link is never dropped for any of this, and the session is still up.
    CHECK(cable_client_is_connected(), "session dropped after bad/unknown input");
}

static void test_silence_drops_session(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    CHECK(cable_client_is_connected(), "session not up");

    s_now_ms += CABLE_SILENCE_MS - 1;
    cable_client_poll();
    CHECK(cable_client_is_connected(), "session dropped early");

    // Any inbound frame refreshes the window — including a bare ping.
    feed("{\"t\":\"ping\"}");
    s_now_ms += CABLE_SILENCE_MS - 1;
    cable_client_poll();
    CHECK(cable_client_is_connected(), "session dropped despite ping");

    s_now_ms += CABLE_SILENCE_MS + 1;
    cable_client_poll();
    CHECK(!cable_client_is_connected(), "session survived 15 s of silence");
    CHECK(s_ev_session_down == 1, "session down event");
    CHECK(!s_log_framing, "log framing not restored to console");
    // Agents are forgotten: a tile is a claim that something is running NOW.
    cable_agent_t agents[8];
    CHECK(cable_client_list_agents(agents, 8) == 0, "agents kept after session down");
    CHECK(cable_client_pending_question() == NULL, "question kept after session down");
}

static void test_host_gone_drops_session(void)
{
    reset_all();
    start_session();
    s_host_present = false;   // cable out / machine asleep
    cable_client_poll();
    CHECK(!cable_client_is_connected(), "session survived host detach");
}

// ── touch-era vocabulary & robustness (v0.2) ───────────────────────────────

#define QUESTION_A2 \
    "{\"t\":\"question\",\"agentId\":\"a2\",\"name\":\"Device firmware voice\",\"id\":\"q2\"," \
    "\"questions\":[{\"key\":\"Ship it?\",\"q\":\"Ship it?\",\"options\":[\"Yes\",\"No\"],\"multi\":false}]}"

static void test_stop_and_scroll_exact(void)
{
    reset_all();
    start_session();
    s_sent_count = 0;
    cable_client_send_stop("a1");
    CHECK(last_json() && strcmp(last_json(), "{\"t\":\"turn.stop\",\"agentId\":\"a1\"}") == 0,
          "turn.stop: %s", last_json() ? last_json() : "(none)");
    cable_client_send_stop("");   // no agent → nothing on the wire
    CHECK(count_json_sent() == 1, "turn.stop sent for empty id");

    cable_client_send_scroll("down", 0, 0, false);
    CHECK(strcmp(last_json(), "{\"t\":\"scroll\",\"phase\":\"down\",\"dy\":0}") == 0,
          "scroll down (zero dy is never elided): %s", last_json());
    cable_client_send_scroll("move", 12, 0, false);
    CHECK(strcmp(last_json(), "{\"t\":\"scroll\",\"phase\":\"move\",\"dy\":12}") == 0,
          "scroll move: %s", last_json());
    cable_client_send_scroll("up", 3, -1800, true);
    CHECK(strcmp(last_json(), "{\"t\":\"scroll\",\"phase\":\"up\",\"dy\":3,\"v\":-1800}") == 0,
          "scroll up (PROTOCOL.md §4.13 example): %s", last_json());
    const int before = count_json_sent();
    cable_client_send_scroll("fling", 1, 0, false);   // not a phase the daemon accepts
    CHECK(count_json_sent() == before, "invalid scroll phase was sent");
}

static void test_answer_resumes_agent_and_clears_question_row(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    feed("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Working\xE2\x80\xA6\"}");
    feed(QUESTION);
    feed("{\"t\":\"notif.replace\",\"items\":[{\"agentId\":\"a1\",\"name\":\"Fix login screen\","
         "\"summary\":\"Which DB?\",\"question\":true}]}");
    CHECK(s_ev_notif_questions == 1, "question row not reported");

    cable_agent_t agents[8];
    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].state, "waiting") == 0, "state before answer '%s'", agents[0].state);

    const int notif_before = s_ev_notif;
    cJSON *answers = cJSON_CreateObject();
    cJSON_AddStringToObject(answers, "Which DB should I use?", "Postgres");
    cable_client_answer("a1", "q1", answers);
    cJSON_Delete(answers);

    cable_client_list_agents(agents, 8);
    CHECK(strcmp(agents[0].state, "running") == 0,
          "answered agent stuck in '%s' (the turn carries on)", agents[0].state);
    CHECK(s_ev_notif == notif_before + 1 && s_ev_notif_questions == 0,
          "question row not cleared on answer (events %d, questions %d)",
          s_ev_notif - notif_before, s_ev_notif_questions);
    cable_notif_t rows[CABLE_NOTIF_MAX];
    CHECK(cable_client_list_notifs(rows, CABLE_NOTIF_MAX) == 0, "notif rows left after answer");
}

static void test_question_queue(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    feed(QUESTION);
    feed(QUESTION_A2);
    CHECK(cable_client_question_count() == 2, "queue holds %d, want 2", cable_client_question_count());
    CHECK(s_ev_question == 2, "question events %d", s_ev_question);

    cable_question_t q;
    CHECK(cable_client_copy_question(&q) && strcmp(q.request_id, "q1") == 0, "head is not the oldest");

    cJSON *answers = cJSON_CreateObject();
    cJSON_AddStringToObject(answers, "Which DB should I use?", "SQLite");
    cable_client_answer("a1", "q1", answers);
    cJSON_Delete(answers);
    CHECK(cable_client_question_count() == 1, "answer did not dequeue");
    CHECK(cable_client_copy_question(&q) && strcmp(q.request_id, "q2") == 0 &&
          strcmp(q.agent_id, "a2") == 0, "second question lost when the first was answered");

    feed("{\"t\":\"question.close\",\"agentId\":\"a2\",\"id\":\"q2\"}");
    CHECK(cable_client_question_count() == 0, "close did not dequeue");
    CHECK(!cable_client_copy_question(&q), "copy succeeded on empty queue");

    // Overflow drops the OLDEST (still pending on the daemon; re-shown later).
    char buf[256];
    for (int i = 0; i < CABLE_Q_PENDING_MAX + 1; i++) {
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"question\",\"agentId\":\"a1\",\"id\":\"r%d\",\"questions\":"
                 "[{\"key\":\"k\",\"q\":\"k?\",\"options\":[\"x\"]}]}", i);
        feed(buf);
    }
    CHECK(cable_client_question_count() == CABLE_Q_PENDING_MAX, "queue overflowed");
    CHECK(cable_client_copy_question(&q) && strcmp(q.request_id, "r1") == 0,
          "overflow kept '%s' at head, want r1", q.request_id);
}

static void test_question_without_key_is_not_shown(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    uint32_t bad0, unk0, bad1, unk1;
    cable_client_counters(&bad0, &unk0);
    feed("{\"t\":\"question\",\"agentId\":\"a1\",\"id\":\"qx\",\"questions\":[{\"q\":\"no key\",\"options\":[\"a\"]}]}");
    cable_client_counters(&bad1, &unk1);
    CHECK(cable_client_question_count() == 0, "unanswerable question queued");
    CHECK(s_ev_question == 0, "blank question screen shown");
    CHECK(bad1 == bad0 + 1, "unanswerable question not counted bad");
}

static void test_notif_seen(void)
{
    reset_all();
    start_session();
    feed("{\"t\":\"notif.replace\",\"items\":["
         "{\"agentId\":\"a1\",\"name\":\"A\",\"summary\":\"done\",\"question\":false},"
         "{\"agentId\":\"a2\",\"name\":\"B\",\"summary\":\"Ship it?\",\"question\":true}]}");
    cable_notif_t rows[CABLE_NOTIF_MAX];
    CHECK(cable_client_list_notifs(rows, CABLE_NOTIF_MAX) == 2, "notif rows");

    uint32_t bad, unknown0, unknown1;
    cable_client_counters(&bad, &unknown0);
    feed("{\"t\":\"notif.seen\",\"agentId\":\"a1\"}");
    cable_client_counters(&bad, &unknown1);
    CHECK(unknown1 == unknown0, "notif.seen counted as unknown");
    CHECK(cable_client_list_notifs(rows, CABLE_NOTIF_MAX) == 1 && strcmp(rows[0].agent_id, "a2") == 0,
          "notif.seen did not drop the a1 row");
    // A question is unread until ANSWERED, not merely seen (§5.19).
    feed("{\"t\":\"notif.seen\",\"agentId\":\"a2\"}");
    CHECK(cable_client_list_notifs(rows, CABLE_NOTIF_MAX) == 1, "seen dropped a question row");
}

static void test_session_down_clears_queue_and_notifs(void)
{
    reset_all();
    start_session();
    feed_agent_list();
    feed(QUESTION);
    feed("{\"t\":\"notif.replace\",\"items\":[{\"agentId\":\"a1\",\"question\":true}]}");
    s_host_present = false;
    cable_client_poll();
    cable_notif_t rows[CABLE_NOTIF_MAX];
    CHECK(cable_client_question_count() == 0, "questions kept after session down");
    CHECK(cable_client_list_notifs(rows, CABLE_NOTIF_MAX) == 0, "notifs kept after session down");
    CHECK(s_ev_notif_questions == 0, "UI not told the unread list emptied");
}

static bool valid_utf8(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int n = *p < 0x80 ? 1 : (*p >> 5) == 0x6 ? 2 : (*p >> 4) == 0xE ? 3 : (*p >> 3) == 0x1E ? 4 : 0;
        if (!n) return false;
        for (int i = 1; i < n; i++) if ((p[i] & 0xC0) != 0x80) return false;
        p += n;
    }
    return true;
}

static void test_utf8_safe_truncation(void)
{
    char out[8];
    cable_utf8_copy(out, 5, "Caf\xC3\xA9!");            // é would be cut after its lead byte
    CHECK(strcmp(out, "Caf") == 0, "2-byte cut: '%s'", out);
    cable_utf8_copy(out, 6, "Caf\xC3\xA9!");
    CHECK(strcmp(out, "Caf\xC3\xA9") == 0, "2-byte fits: '%s'", out);
    cable_utf8_copy(out, 6, "Wait\xE2\x80\xA6");       // … (3 bytes) cut after 1
    CHECK(strcmp(out, "Wait") == 0, "3-byte cut: '%s'", out);
    cable_utf8_copy(out, 7, "ok \xF0\x9F\x98\x80");      // 4-byte emoji cut after 3
    CHECK(strcmp(out, "ok ") == 0, "4-byte cut: '%s'", out);
    cable_utf8_copy(out, sizeof(out), "short");
    CHECK(strcmp(out, "short") == 0, "no-op copy: '%s'", out);
    cable_utf8_copy(out, sizeof(out), NULL);
    CHECK(out[0] == '\0', "NULL source");

    // End to end: an agent name longer than CABLE_NAME_MAX, accented at the cut.
    reset_all();
    start_session();
    char name[128] = "";
    for (int i = 0; i < 30; i++) strcat(name, "\xC3\xA9");   // 60 bytes of "é"
    char row[256];
    snprintf(row, sizeof(row), "{\"t\":\"agent\",\"id\":\"u1\",\"name\":\"xx%s\"}", name);   // "xx" puts the cut mid-"é"
    feed("{\"t\":\"agents.begin\"}");
    feed(row);
    feed("{\"t\":\"agents.end\",\"total\":1,\"tab\":\"t1\"}");
    cable_agent_t agents[8];
    CHECK(cable_client_list_agents(agents, 8) == 1, "utf-8 agent row");
    CHECK(valid_utf8(agents[0].name), "agent name truncated mid-character");
    CHECK(strlen(agents[0].name) == CABLE_NAME_MAX - 2, "name length %zu", strlen(agents[0].name));
}

// The same structs, seen from a translation unit that included <limits.h>
// first (layout_tu.c), must have the same size as here. Regression test for
// the NAME_MAX collision that shifted every field after `name` in ui.c.
size_t layout_agent_size(void);
size_t layout_question_size(void);
size_t layout_notif_size(void);

static void test_struct_layout_is_include_order_independent(void)
{
    CHECK(layout_agent_size() == sizeof(cable_agent_t), "cable_agent_t %zu vs %zu",
          layout_agent_size(), sizeof(cable_agent_t));
    CHECK(layout_question_size() == sizeof(cable_question_t), "cable_question_t %zu vs %zu",
          layout_question_size(), sizeof(cable_question_t));
    CHECK(layout_notif_size() == sizeof(cable_notif_t), "cable_notif_t %zu vs %zu",
          layout_notif_size(), sizeof(cable_notif_t));
    CHECK(sizeof(((cable_agent_t *)0)->name) == 40, "agent name cap %zu (PROTOCOL.md §11: 40)",
          sizeof(((cable_agent_t *)0)->name));
}

int main(void)
{
    test_struct_layout_is_include_order_independent();
    test_hello_exact();
    test_stop_and_scroll_exact();
    test_answer_resumes_agent_and_clears_question_row();
    test_question_queue();
    test_question_without_key_is_not_shown();
    test_notif_seen();
    test_session_down_clears_queue_and_notifs();
    test_utf8_safe_truncation();
    test_hello_cadence();
    test_ping_pong();
    test_agents_list_after_welcome();
    test_focus_and_open_exact();
    test_answer_exact();
    test_answer_escaping_and_verbatim_key();
    test_agent_list_parse();
    test_agent_list_refresh_keeps_state();
    test_agent_list_no_window();
    test_nested_p_envelope();
    test_turn_lifecycle();
    test_turn_started_flag();
    test_summary_variants();
    test_notif_replace();
    test_question_flow();
    test_focus_inbound();
    test_fw_offer_never_answered();
    test_unknown_and_bad_never_fatal();
    test_silence_drops_session();
    test_host_gone_drops_session();
    printf("test_messages: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
