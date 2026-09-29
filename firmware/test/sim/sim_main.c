// Host simulator scenario: the REAL ui.c + cable_client.c + LVGL 9.2.2 on a
// virtual 240×240 panel, driven like a person would drive the dial, against
// a scripted daemon speaking the real JSON vocabulary. Every outbound frame
// is checked; every screen is saved as a screenshot.
//
//   usage: sim <out_dir>      exit status = number of failed checks

#include <stdlib.h>
#include <string.h>

#include "cable_client.h"
#include "cable_frame.h"
#include "display.h"
#include "settings.h"
#include "sim.h"
#include "touch.h"
#include "ui.h"

static int         s_fail, s_checks;
static const char *s_out = ".";
static int         s_shot;
static bool        s_daemon_alive;

#define CHECK(cond, ...)                                                         \
    do {                                                                         \
        s_checks++;                                                              \
        if (!(cond)) {                                                           \
            s_fail++;                                                            \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                          \
            printf(__VA_ARGS__);                                                 \
            printf("\n");                                                        \
        }                                                                        \
    } while (0)

// ── driving time ────────────────────────────────────────────────────────────

// Advance simulated time in 10 ms UI-loop steps. Like the device, the session
// machine ticks every 100 ms; a live daemon pings every 5 s.
static void step(uint32_t ms)
{
    static uint32_t since_poll, since_ping;
    for (uint32_t t = 0; t < ms; t += 10) {
        sim_clock_advance_ms(10);
        since_poll += 10;
        since_ping += 10;
        if (since_poll >= 100) { since_poll = 0; cable_client_poll(); }
        if (s_daemon_alive && since_ping >= 5000) {
            since_ping = 0;
            const char *ping = "{\"t\":\"ping\"}";
            cable_client_handle_frame(1, CABLE_TYPE_JSON, (const uint8_t *)ping, strlen(ping), NULL);
        }
        ui_sim_step();
    }
}

static void daemon_says(const char *json)
{
    cable_client_handle_frame(1, CABLE_TYPE_JSON, (const uint8_t *)json, strlen(json), NULL);
}

static void shot(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%02d_%s.ppm", s_out, ++s_shot, name);
    CHECK(sim_screenshot(path), "screenshot %s", path);
}

// ── outbound frames ─────────────────────────────────────────────────────────

static int mark(void) { return sim_sent_count; }

// The last frame of type `t` sent since `from`, or NULL.
static const char *sent_since(int from, const char *t)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"t\":\"%s\"", t);
    for (int i = sim_sent_count - 1; i >= from; i--) {
        if (strstr(sim_sent[i], needle)) return sim_sent[i];
    }
    return NULL;
}

static int count_since(int from, const char *t)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"t\":\"%s\"", t);
    int n = 0;
    for (int i = from; i < sim_sent_count; i++) if (strstr(sim_sent[i], needle)) n++;
    return n;
}

// ── finding things on screen ────────────────────────────────────────────────

static bool visible(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o)) if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}

// First visible label on the active screen (or the top layer) whose text
// contains `text`.
static lv_obj_t *find_label_in(lv_obj_t *root, const char *text, bool exact)
{
    if (!root || !visible(root)) return NULL;
    if (lv_obj_check_type(root, &lv_label_class)) {
        const char *t = lv_label_get_text(root);
        if (exact ? strcmp(t, text) == 0 : strstr(t, text) != NULL) return root;
    }
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *hit = find_label_in(lv_obj_get_child(root, (int32_t)i), text, exact);
        if (hit) return hit;
    }
    return NULL;
}

static lv_obj_t *find_label_ex(const char *text, bool exact)
{
    // The top layer (toasts) sits above the screen: search it last so a
    // toast never shadows a button in the checks.
    lv_obj_t *hit = find_label_in(lv_screen_active(), text, exact);
    return hit ? hit : find_label_in(lv_layer_top(), text, exact);
}

static lv_obj_t *find_label(const char *text) { return find_label_ex(text, false); }

static bool on_screen(const char *text) { return find_label(text) != NULL; }

// The flag that outlives a finished turn is a colour, so the scenario reads the
// panel: green (COL_DONE 0x22c55e) for work to look at, red (COL_ERROR
// 0xef4444) for a turn that failed. The bounds reject the white of a current
// page dot and the grey of an idle one — a flag is neither.
static bool pixel_flag_green(int x, int y)
{
    const uint16_t c = sim_pixel(x, y);
    const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return g >= 40 && r <= 12 && b <= 20;
}

static bool pixel_flag_red(int x, int y)
{
    const uint16_t c = sim_pixel(x, y);
    const int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return r >= 25 && g <= 20 && b <= 12;
}

// The page dots sit in one band under the card and the row RE-CENTRES as the
// current page widens into a pill, so a fixed pixel would read the gap. This
// scans the band instead: 1 = a green flag, 2 = a red one, 0 = none. The band
// stops above the action chip, whose text is red on a running agent.
static int dots_flag(void)
{
    for (int y = 174; y <= 185; y++) {
        for (int x = 92; x <= 148; x++) {
            if (pixel_flag_green(x, y)) return 1;
            if (pixel_flag_red(x, y)) return 2;
        }
    }
    return 0;
}

static void center_of(lv_obj_t *o, int *x, int *y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

// ── fingers ─────────────────────────────────────────────────────────────────

static void tap_at(int x, int y)
{
    sim_touch(x, y, true);
    step(80);
    sim_touch(x, y, false);
    step(350);   // a person does not tap faster than a scroll/screen animation
}

// Inside the round glass (r = 120, minus a finger's margin)?
static bool reachable(int x, int y)
{
    const int dx = x - 120, dy = y - 120;
    return dx * dx + dy * dy <= 112 * 112;
}

// Tap the label showing `text` (LVGL routes the press to its clickable
// parent). `exact` matches the whole text.
static bool tap_label(const char *text, bool exact)
{
    lv_obj_t *l = find_label_ex(text, exact);
    if (!l) { printf("  (no '%s' on screen)\n", text); return false; }
    int x, y;
    center_of(l, &x, &y);
    // A control the finger cannot reach is a UI bug, not something to tap
    // anyway at y = 281 (LVGL would just clamp the point).
    if (!reachable(x, y)) { printf("  ('%s' is off the round screen at %d,%d)\n", text, x, y); return false; }
    tap_at(x, y);
    return true;
}

static bool tap_text(const char *text) { return tap_label(text, false); }

static void swipe(int x0, int y0, int x1, int y1)
{
    const int n = 8;
    sim_touch(x0, y0, true);
    step(40);
    for (int i = 1; i <= n; i++) {
        sim_touch(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, true);
        step(20);
    }
    sim_touch(x1, y1, false);
    step(300);   // let any screen animation finish
}

// ── the scenario ────────────────────────────────────────────────────────────

const char *ui_sim_screen(void);   // ui.c, simulator builds only
extern int sim_lamp_saves;         // sim_platform.c

static bool screen_is(const char *name) { return strcmp(ui_sim_screen(), name) == 0; }

static void hold_at(int x, int y, uint32_t ms)
{
    sim_touch(x, y, true);
    step(ms);
    sim_touch(x, y, false);
    step(400);
}

static void drag(int x0, int y0, int x1, int y1)
{
    sim_touch(x0, y0, true);
    step(40);
    for (int i = 1; i <= 10; i++) {
        sim_touch(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10, true);
        step(30);
    }
    sim_touch(x1, y1, false);
    step(200);
}

static void scenario(void)
{
    // Boot → "Not connected".
    shot("boot");
    CHECK(on_screen("Harness C3"), "boot screen");
    step(1500);
    shot("offline");
    CHECK(on_screen("Non connecté"), "offline screen after boot");
    CHECK(sent_since(0, "hello"), "hello sent at boot");

    // Welcome → home, and the dial asks for the list explicitly.
    int m = mark();
    s_daemon_alive = true;
    daemon_says("{\"t\":\"welcome\",\"proto\":3,\"app\":\"harness\","
                "\"machine\":{\"id\":\"mac-local\",\"name\":\"MacBook Pro\"},\"selected\":\"mac-local\"}");
    step(300);
    CHECK(sent_since(m, "agents.list"), "agents.list after first welcome");
    daemon_says("{\"t\":\"agents.begin\"}");
    daemon_says("{\"t\":\"agent\",\"id\":\"a1\",\"name\":\"Réparer l'écran de connexion\",\"engine\":\"claude\","
                "\"machineId\":\"mac-local\",\"machine\":\"MacBook Pro\"}");
    daemon_says("{\"t\":\"agent\",\"id\":\"a2\",\"name\":\"Firmware C3\",\"engine\":\"codex\","
                "\"machineId\":\"mac-local\",\"machine\":\"MacBook Pro\"}");
    daemon_says("{\"t\":\"agent\",\"id\":\"a3\",\"name\":\"Docs\",\"engine\":\"cursor\"}");
    daemon_says("{\"t\":\"agents.end\",\"total\":7,\"tab\":\"t1\"}");
    daemon_says("{\"t\":\"notif.replace\",\"items\":[]}");
    step(400);
    shot("home");
    CHECK(on_screen("Réparer l'écran"), "agent 1 on home (accents intact)");
    CHECK(on_screen("MacBook Pro"), "machine name header");
    CHECK(on_screen("7"), "fleet badge");

    // A turn starts: blue ring, STOP chip, status line with "…".
    daemon_says("{\"t\":\"turn.started\",\"agentId\":\"a1\",\"text\":\"Compilation du firmware…\"}");
    step(500);
    shot("home_running");
    CHECK(on_screen("En cours"), "running state label");
    CHECK(on_screen("Compilation du firmware"), "status line");
    CHECK(on_screen("Stop"), "stop chip on a running agent");

    // STOP: first tap arms, second sends turn.stop.
    m = mark();
    tap_text("Stop");
    CHECK(on_screen("Confirmer"), "stop armed");
    CHECK(!sent_since(m, "turn.stop"), "no stop on the first tap");
    shot("home_stop_armed");
    tap_text("Confirmer");
    const char *stop = sent_since(m, "turn.stop");
    CHECK(stop && strcmp(stop, "{\"t\":\"turn.stop\",\"agentId\":\"a1\"}") == 0, "turn.stop: %s", stop ? stop : "(none)");
    step(3200);   // toast gone

    // Swipe ← → next agent, `focus` sent, and the swipe's release is NOT a tap.
    m = mark();
    swipe(190, 120, 50, 120);
    const char *focus = sent_since(m, "focus");
    CHECK(focus && strstr(focus, "\"agentId\":\"a2\""), "focus a2 after swipe: %s", focus ? focus : "(none)");
    CHECK(!sent_since(m, "agent.open"), "a swipe that ends on the card is not a tap");
    CHECK(on_screen("Firmware C3"), "agent 2 shown");
    shot("home_agent2");
    swipe(50, 120, 190, 120);   // back to a1
    CHECK(on_screen("Réparer"), "swipe → back to agent 1");

    // Chevron tap = next agent too.
    m = mark();
    tap_at(211, 116);
    CHECK(sent_since(m, "focus") && strstr(sent_since(m, "focus"), "a2"), "chevron › selects a2");
    swipe(50, 120, 190, 120);

    // Tap the card → agent.open with NO reason (a person's tap).
    m = mark();
    tap_at(120, 116);
    const char *open = sent_since(m, "agent.open");
    CHECK(open && strcmp(open, "{\"t\":\"agent.open\",\"agentId\":\"a1\"}") == 0, "agent.open: %s", open ? open : "(none)");
    CHECK(on_screen("Ouvert sur l'ordinateur"), "open toast");
    shot("home_open_toast");
    step(3200);

    // A two-item question: single then multi. The dial sends agent.open
    // reason=question itself and takes the face.
    m = mark();
    daemon_says("{\"t\":\"question\",\"agentId\":\"a2\",\"name\":\"Firmware C3\",\"id\":\"q1\",\"questions\":["
                "{\"key\":\"Quelle base ?\",\"q\":\"Quelle base de données dois-je utiliser pour le cache ?\","
                "\"options\":[\"Postgres\",\"SQLite\",\"Aucune\"],\"multi\":false},"
                "{\"key\":\"Cibles\",\"q\":\"Pour quelles cibles ?\",\"options\":[\"ESP32-C3\",\"ESP32-S3\",\"Linux\",\"macOS\"],\"multi\":true}]}");
    step(400);
    CHECK(sent_since(m, "agent.open") && strstr(sent_since(m, "agent.open"), "\"reason\":\"question\""),
          "agent.open reason=question");
    CHECK(on_screen("Quelle base de données"), "question screen shown");
    shot("question");

    // Keepalive welcome while answering: stays on the question.
    daemon_says("{\"t\":\"welcome\",\"proto\":3,\"machine\":{\"id\":\"mac-local\",\"name\":\"MacBook Pro\"}}");
    step(200);
    CHECK(on_screen("Quelle base de données"), "keepalive welcome keeps the question on screen");

    CHECK(tap_text("SQLite"), "tap option");
    shot("question_selected");
    CHECK(tap_label(LV_SYMBOL_RIGHT, true), "→ to the next item");
    CHECK(on_screen("Pour quelles cibles"), "second item");
    tap_text("ESP32-C3");
    tap_text("Linux");
    shot("question_multi");
    m = mark();
    CHECK(tap_label(LV_SYMBOL_OK, true), "✓ send");
    step(300);
    const char *answer = sent_since(m, "answer");
    CHECK(answer && strcmp(answer,
          "{\"t\":\"answer\",\"agentId\":\"a2\",\"requestId\":\"q1\","
          "\"answers\":{\"Quelle base ?\":\"SQLite\",\"Cibles\":\"ESP32-C3, Linux\"}}") == 0,
          "answer exact: %s", answer ? answer : "(none)");
    CHECK(on_screen("Réponse envoyée"), "answer toast");
    shot("answered");
    step(3200);

    // Dismiss → amber chip on home → tap it → back on the question.
    daemon_says("{\"t\":\"question\",\"agentId\":\"a3\",\"id\":\"q2\",\"questions\":["
                "{\"key\":\"ok\",\"q\":\"Publier la doc ?\",\"options\":[\"Oui\",\"Non\"]}]}");
    step(300);
    CHECK(on_screen("Publier la doc"), "second question shown");
    swipe(40, 120, 200, 120);   // sideways swipe = later
    CHECK(on_screen("? Question"), "amber chip after dismissing");
    shot("home_question_chip");
    tap_text("? Question");
    CHECK(on_screen("Publier la doc"), "chip reopens the question");

    // Closed elsewhere while on screen → back home with a note.
    daemon_says("{\"t\":\"question.close\",\"agentId\":\"a3\",\"id\":\"q2\"}");
    step(300);
    CHECK(!on_screen("Publier la doc") && on_screen("traitée ailleurs"), "question.close leaves the screen");
    step(3200);
    CHECK(!on_screen("? Question"), "no chip once nothing is pending");

    // Free-text question: no options → only ✕, and a hint.
    daemon_says("{\"t\":\"question\",\"agentId\":\"a1\",\"id\":\"q3\",\"questions\":["
                "{\"key\":\"why\",\"q\":\"Pourquoi ce choix ?\",\"options\":[]}]}");
    step(300);
    CHECK(on_screen("répondez sur l'ordinateur"), "free-text hint");
    shot("question_free_text");
    CHECK(tap_label(LV_SYMBOL_CLOSE, true), "✕ dismisses");
    daemon_says("{\"t\":\"question.close\",\"agentId\":\"a1\",\"id\":\"q3\"}");
    step(300);

    // A finished turn announces itself (no buzzer on this board: the toast).
    daemon_says("{\"t\":\"summary\",\"agentId\":\"a2\",\"recap\":\"Build OK, 0 warning\",\"text\":\"…\"}");
    step(200);
    CHECK(on_screen("Build OK"), "summary toast");
    shot("summary_toast");
    step(3200);

    // …and it leaves a flag the toast never did: three seconds later the toast
    // is gone and this board has no buzzer, so a person who steps away would
    // come back to a silent dial. The agent keeps the flag until its card is
    // touched — green for a finished turn, red for one that failed. a2 is not
    // the card on screen, so it shows first on its PAGE DOT: that is how one
    // finds WHICH of four agents moved without walking the carousel.
    CHECK(dots_flag() == 1, "a2's page dot carries the flag");
    shot("unseen_page_dot");

    swipe(190, 120, 50, 120);   // over to a2
    CHECK(pixel_flag_green(187, 177), "the flag is on a2's card");
    shot("unseen_flag");

    // The touch IS the acknowledgement: opening the agent spends the flag.
    tap_at(120, 116);
    step(300);
    CHECK(!pixel_flag_green(187, 177), "touching the card spends the flag");
    CHECK(dots_flag() == 0, "…and its page dot too");
    shot("unseen_flag_spent");
    swipe(50, 120, 190, 120);   // back to a1

    // A turn that FAILED keeps a red one — the colour says which happened.
    daemon_says("{\"t\":\"turn.error\",\"agentId\":\"a2\",\"message\":\"Quota dépassé\"}");
    step(200);
    CHECK(on_screen("Quota dépassé"), "error toast");
    CHECK(dots_flag() == 2, "a failed turn flags red on its page dot");
    shot("unseen_flag_error");
    step(3200);

    // Pull ↓ → settings; the brightness slider; push ↑ → home.
    swipe(120, 40, 120, 200);
    CHECK(on_screen("Réglages"), "settings screen");
    CHECK(on_screen("cst816d"), "touch chip shown");
    shot("settings");
    const int saves = s_brightness_saves;
    sim_touch(60, 118, true); step(60);
    for (int x = 60; x <= 170; x += 10) { sim_touch(x, 118, true); step(30); }
    sim_touch(170, 118, false); step(100);
    CHECK(s_brightness_saves == saves + 1, "brightness saved once on release");
    CHECK(settings_brightness() > 80, "brightness raised (%d)", settings_brightness());
    CHECK(on_screen("Réglages"), "slider drag is not a swipe");
    swipe(120, 200, 120, 40);
    CHECK(on_screen("Réparer") || on_screen("Firmware") || on_screen("Docs"), "back home");

    // Push ↑ → scrollpad: down / move… / up with a velocity.
    swipe(120, 200, 120, 60);
    CHECK(on_screen("Défilement"), "scrollpad screen");
    shot("scrollpad");
    m = mark();
    sim_touch(120, 60, true); step(40);
    for (int y = 60; y <= 180; y += 15) { sim_touch(120, y, true); step(20); }
    sim_touch(120, 180, false); step(60);
    const char *down = sent_since(m, "scroll");
    CHECK(count_since(m, "scroll") >= 3, "scroll frames: %d", count_since(m, "scroll"));
    CHECK(strstr(sim_sent[m], "\"phase\":\"down\",\"dy\":0") != NULL, "first scroll is down/0: %s", sim_sent[m]);
    CHECK(down && strstr(down, "\"phase\":\"up\"") && strstr(down, "\"v\":"), "last is up with v: %s", down ? down : "");
    int total = 0;
    for (int i = m; i < sim_sent_count; i++) {
        const char *d = strstr(sim_sent[i], "\"dy\":");
        if (strstr(sim_sent[i], "\"scroll\"") && d) total += atoi(d + 5);
    }
    CHECK(total == 120, "scroll dy adds up to the finger travel (%d)", total);
    // A sideways swipe leaves the pad.
    swipe(40, 120, 200, 120);
    CHECK(!on_screen("Défilement"), "sideways swipe leaves the scrollpad");

    // ── Lamp mode ──
    // Hold the card → the dial becomes a lamp, at the lamp's own level.
    hold_at(120, 116, 900);
    CHECK(screen_is("lamp"), "hold on the card opens the lamp (%s)", ui_sim_screen());
    CHECK(on_screen("intensité"), "first time: how to use it");
    step(1500);
    CHECK(display_get_backlight() == settings_lamp_level(), "backlight at the lamp level (%d vs %d)",
          display_get_backlight(), settings_lamp_level());
    shot("lamp");
    // Drag ↑ → brighter, live; saved once, on release.
    int saves0 = sim_lamp_saves;
    drag(120, 170, 120, 90);
    CHECK(settings_lamp_level() == 100 && display_get_backlight() == 100, "drag up: brighter (%d)", settings_lamp_level());
    CHECK(sim_lamp_saves == saves0 + 1, "saved once (%d)", sim_lamp_saves - saves0);
    CHECK(screen_is("lamp"), "a drag is not a tap");
    drag(120, 90, 120, 150);
    CHECK(settings_lamp_level() == 70, "drag down: dimmer (%d)", settings_lamp_level());
    // Drag → → cooler tone (40 px per step).
    drag(60, 120, 150, 120);
    CHECK(settings_lamp_warmth() == 3, "drag right: cooler tone (%d)", settings_lamp_warmth());
    shot("lamp_daylight");
    drag(180, 120, 20, 120);
    CHECK(settings_lamp_warmth() == 0, "drag left: warmest tone (%d)", settings_lamp_warmth());
    shot("lamp_candle");
    // A lamp does not dim, and a touch on it is never taken for a wake-up.
    step(70000);
    CHECK(display_get_backlight() == settings_lamp_level(), "no dimming in lamp mode (%d)", display_get_backlight());
    // A question takes the face, then hands it back to the lamp.
    daemon_says("{\"t\":\"question\",\"agentId\":\"a1\",\"id\":\"q7\",\"questions\":[{\"key\":\"Go ?\",\"q\":\"On déploie ?\",\"options\":[\"Oui\",\"Non\"],\"multi\":false}]}");
    step(400);
    CHECK(screen_is("question") && on_screen("On déploie"), "a question takes the face from the lamp");
    tap_text("Oui");
    CHECK(tap_label(LV_SYMBOL_OK, true), "answer it");
    step(600);
    CHECK(screen_is("lamp"), "answered: back to the lamp (%s)", ui_sim_screen());
    // Tap → home.
    tap_at(120, 120);
    step(400);
    CHECK(screen_is("home"), "tap leaves the lamp (%s)", ui_sim_screen());
    CHECK(display_get_backlight() == settings_brightness(), "home: UI brightness again (%d)", display_get_backlight());
    // Settings → Lamp button → the lamp; tap → home.
    step(3200);   // the "answer sent" toast sits where the Lamp button is: let it go
    swipe(120, 40, 120, 200);
    CHECK(tap_text("Lampe"), "settings has a Lamp button");
    step(300);
    CHECK(screen_is("lamp"), "Lamp button opens the lamp");
    tap_at(120, 120);
    step(400);

    // Idle → dim → a tap only wakes (no action).
    step(61000);
    CHECK(display_get_backlight() < settings_brightness(), "dimmed after 60 s (%d)", display_get_backlight());
    shot("dimmed");
    m = mark();
    tap_at(120, 116);   // on the card
    CHECK(display_get_backlight() == settings_brightness(), "tap wakes the screen");
    CHECK(!sent_since(m, "agent.open"), "the waking tap does nothing else");

    // The daemon goes quiet → session down after 15 s → "Not connected".
    s_daemon_alive = false;
    step(16000);
    CHECK(on_screen("Non connecté"), "offline after 15 s of silence");
    shot("offline_again");
    // A lamp needs no computer: hold on the offline screen.
    hold_at(120, 120, 900);
    CHECK(screen_is("lamp"), "offline: hold opens the lamp (%s)", ui_sim_screen());
    tap_at(120, 120);
    step(400);
    CHECK(screen_is("offline"), "leaving the lamp offline goes back to offline (%s)", ui_sim_screen());
}

int main(int argc, char **argv)
{
    if (argc > 1) s_out = argv[1];
    sim_verbose = getenv("SIM_VERBOSE") != NULL;

    sim_display_init();
    cable_client_init();
    cable_client_set_identity("0.2.0-c3", CABLE_HW_NAME "+cst816d", "28:84:85:90:5F:78");
    touch_register_lvgl(display_get());
    if (!ui_init("0.2.0-c3", true)) {
        printf("ui_init failed\n");
        return 1;
    }
    scenario();

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("sim: %d checks, %d failures, %d screenshots (LVGL pool: max %u of %u B = %u%%, %d-bit build)\n",
           s_checks, s_fail, s_shot, (unsigned)mon.max_used, (unsigned)LV_MEM_SIZE,
           (unsigned)(100u * mon.max_used / LV_MEM_SIZE), (int)(8 * sizeof(void *)));
    // Keep a real margin on the device: a full pool is an LVGL assert.
    if (sizeof(void *) == 4 && mon.max_used > LV_MEM_SIZE * 85 / 100) {
        printf("FAIL: LVGL pool above 85%% on the device build\n");
        return 1;
    }
    return s_fail ? 1 : 0;
}
