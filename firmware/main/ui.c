// UI: LVGL v9 screens for the 240×240 round touch panel. See ui.h and
// SPEC.md §6.
//
// THREADING. cable_client callbacks run ON THE LINK READER TASK and must never
// touch LVGL. They either raise a "dirty" bit (agents / unread list /
// questions / session: the UI re-reads the truth from cable_client under its
// lock, so a burst of messages can never be lost to a full queue) or post a
// small event carrying text that exists nowhere else (toasts, finished-turn
// recaps, focus). The UI task — and only it — owns LVGL, under display_lock().
//
// GESTURES (home screen): swipe ← / → = next / previous agent (sends
// `focus`), tap the card = `agent.open`, pull ↓ = settings, push ↑ =
// scrollpad. Question screen: tap options, ✓ sends, ✕ or a sideways swipe
// dismisses (the question stays pending: the amber chip on home brings it
// back). A tap on a dark or dimmed screen only wakes it.
//
// Buttons (optional; the ESP32-2424S012C has only BOOT = button B):
//   A short = next agent / next option      A long = open agent / confirm
//   B short = back / dismiss / open question B long = screen off / on

#include "ui.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "buttons.h"
#include "buzzer.h"
#include "cable_client.h"
#include "cable_link.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "fonts/fonts.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "settings.h"
#include "touch.h"

static const char *TAG = "ui";

#if CONFIG_HARNESS_LANG_EN
#define TR(fr, en) (en)
#else
#define TR(fr, en) (fr)
#endif

// ── geometry (240 px circle, centre 120/120) ───────────────────────────────
// The status ring's inner edge sits at r ≈ 110: everything interactive stays
// inside it, critical text inside the ~170 px inscribed square.
#define SCR_W   240
#define RING_D  232
#define RING_W  6
#define CARD_W  160
#define TEXT_W  156

// ── palette: low-saturation dark; ring colours per agent state ─────────────
#define COL_BG       0x0b0f14
#define COL_SURFACE  0x141b23
#define COL_SURFACE2 0x1c242e
#define COL_LINE     0x2b3642
#define COL_TEXT     0xd7dde5
#define COL_DIM      0x8b98a5
#define COL_IDLE     0x5b6470   // grey
#define COL_RUNNING  0x3b82f6   // blue
#define COL_WAITING  0xf59e0b   // amber
#define COL_DONE     0x22c55e   // green
#define COL_ERROR    0xef4444   // red
#define COL_ACCENT   0x60a5fa

// ── timings ─────────────────────────────────────────────────────────────────
#define UI_LOOP_MS       10
#define BOOT_SCREEN_MS   1200
#define TOAST_MS         3000
#define STOP_CONFIRM_MS  3000
#define PULSE_PERIOD_MS  600     // ring blink while a question waits: 2 redraws / 1.2 s
#define BUSY_PERIOD_MS   400     // "Working…" dots: a one-line redraw
#define SCROLL_SEND_MS   30      // scrollpad: at most ~33 moves / s on the wire
#define ALIVE_EVERY_MS   60000   // PROTOCOL.md §9 heartbeat line
#define SCREEN_ANIM_MS   180

// ── cross-task plumbing ─────────────────────────────────────────────────────

enum {
    DIRTY_AGENTS    = 1u << 0,
    DIRTY_NOTIF     = 1u << 1,
    DIRTY_QUESTIONS = 1u << 2,
    DIRTY_SESSION   = 1u << 3,
};
static atomic_uint s_dirty;

typedef enum {
    UI_EV_SESSION,       // carries the machine name
    UI_EV_AGENT_EVENT,   // a finished turn to announce (recap text)
    UI_EV_QUESTION,      // a NEW question arrived (beep + show)
    UI_EV_TOAST,
    UI_EV_FOCUS,
} ui_ev_type_t;

typedef struct {
    ui_ev_type_t type;
    union {
        struct { bool up; char name[CABLE_NAME_MAX]; } session;
        struct { char id[ID_MAX]; char text[96]; bool notify, beep; } agent;
        struct { char text[112]; } toast;
        struct { char id[ID_MAX]; } focus;
    } d;
} ui_ev_t;

static QueueHandle_t s_btn_queue;
static QueueHandle_t s_ev_queue;

// ── UI state (UI task only) ─────────────────────────────────────────────────

typedef enum { SCR_BOOT, SCR_OFFLINE, SCR_HOME, SCR_QUESTION, SCR_SETTINGS, SCR_PAD } screen_t;

static screen_t s_screen = SCR_BOOT;
static bool     s_has_touch;
static bool     s_connected;
static char     s_machine_name[CABLE_NAME_MAX];
static char     s_fw[40];

static cable_agent_t s_agents[CABLE_MAX_AGENTS];
static int           s_agent_count;
static int           s_index;               // carousel position
static int           s_fleet_total;
static bool          s_has_window;
static int           s_notif_questions;     // unread rows that are questions
static int           s_q_pending;           // questions queued in cable_client

// The question on screen: a private COPY (the link task may rewrite the
// queue at any moment), plus the answers being put together.
static cable_question_t s_q;
static bool             s_q_valid;
static int              s_q_index;                  // which item of the request
static int              s_q_single;                 // chosen option, -1 = none
static bool             s_q_multi[CABLE_OPT_MAX];
static int              s_q_cursor;                 // button navigation
static cJSON           *s_answers;

static bool    s_forced_off;       // BOOT long press
static int64_t s_last_alive_us;

// ── LVGL objects ────────────────────────────────────────────────────────────

static lv_obj_t *s_scr_boot, *s_scr_offline, *s_scr_home, *s_scr_question, *s_scr_settings,
                *s_scr_pad;

// home
static lv_obj_t   *s_ring, *s_h_header, *s_h_state, *s_badge, *s_badge_label;
static lv_obj_t   *s_card, *s_h_name, *s_h_sub, *s_h_summary;
static lv_obj_t   *s_chev_l, *s_chev_r, *s_dots, *s_action, *s_action_label;
static lv_timer_t *s_pulse_timer, *s_busy_timer, *s_stop_timer;
static bool        s_pulse_on = true;
static int         s_busy_phase;
static bool        s_stop_armed;

// question
static lv_obj_t *s_q_col, *s_q_header, *s_q_text, *s_q_opts, *s_q_send, *s_q_close;

// settings
static lv_obj_t *s_set_slider, *s_set_value, *s_set_info, *s_set_dot;

// scrollpad
static lv_obj_t *s_pad_ring;

// toast
static lv_obj_t   *s_toast;
static lv_timer_t *s_toast_timer;

// ── small helpers ───────────────────────────────────────────────────────────

static uint32_t color_of_state(const char *state)
{
    if (strcmp(state, "running") == 0) return COL_RUNNING;
    if (strcmp(state, "waiting") == 0) return COL_WAITING;
    if (strcmp(state, "done") == 0) return COL_DONE;
    if (strcmp(state, "error") == 0) return COL_ERROR;
    return COL_IDLE;
}

static const char *label_of_state(const char *state)
{
    if (strcmp(state, "running") == 0) return TR("En cours", "Working");
    if (strcmp(state, "waiting") == 0) return TR("Attend une réponse", "Needs you");
    if (strcmp(state, "done") == 0) return TR("Terminé", "Done");
    if (strcmp(state, "error") == 0) return TR("Erreur", "Error");
    return TR("Inactif", "Idle");
}

// LVGL still sends CLICKED on the release that ends a swipe (only a SCROLL
// suppresses it). The gesture direction stays set until the next press, so
// every tap handler asks this first.
static bool gesture_in_progress(void)
{
    lv_indev_t *indev = lv_indev_active();
    return indev && lv_indev_get_gesture_dir(indev) != LV_DIR_NONE;
}

static void no_scroll(lv_obj_t *o)
{
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
}

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *new_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, &font_ui_14, 0);   // inherited by every child
    lv_obj_set_style_text_color(scr, lv_color_hex(COL_TEXT), 0);
    no_scroll(scr);
    return scr;
}

// A plain, transparent, non-interactive container.
static lv_obj_t *new_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    no_scroll(o);
    return o;
}

static lv_obj_t *new_ring(lv_obj_t *parent, int diameter, int width, uint32_t color)
{
    lv_obj_t *r = new_box(parent);
    lv_obj_set_size(r, diameter, diameter);
    lv_obj_center(r);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r, width, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(color), 0);
    return r;
}

static lv_obj_t *new_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                           uint32_t color, int width, lv_align_t align, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, align, x, y);
    return l;
}

// A rounded, finger-sized button with a centred label.
static lv_obj_t *new_button(lv_obj_t *parent, const char *text, int w, int h, uint32_t bg,
                            uint32_t fg, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_LINE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static void load(lv_obj_t *scr, screen_t which, lv_screen_load_anim_t anim)
{
    // A glitch on the panel's SPI lines can flip its scan direction and the
    // picture comes out mirrored: rewriting the orientation during a screen
    // transition is invisible to the eye, and heals it (see display.h).
    display_reassert_orientation();
    s_screen = which;
    if (lv_screen_active() == scr) return;
    if (anim == LV_SCR_LOAD_ANIM_NONE) lv_screen_load(scr);
    else lv_screen_load_anim(scr, anim, SCREEN_ANIM_MS, 0, false);
}

// ── power: dim, sleep, wake ─────────────────────────────────────────────────

static int dim_level(void)
{
    const int b = settings_brightness() / 4;
    return b > 5 ? b : 5;
}

// The brightness the screen SHOULD have right now.
static int power_target(void)
{
    if (s_forced_off) return 0;
    const uint32_t idle_ms = lv_display_get_inactive_time(NULL);
    // A waiting question never lets the screen go fully dark.
    if (s_q_pending == 0 && CONFIG_HARNESS_OFF_AFTER_S > 0 &&
        idle_ms >= (uint32_t)CONFIG_HARNESS_OFF_AFTER_S * 1000u) {
        return 0;
    }
    if (CONFIG_HARNESS_DIM_AFTER_S > 0 && idle_ms >= (uint32_t)CONFIG_HARNESS_DIM_AFTER_S * 1000u) {
        return dim_level();
    }
    return settings_brightness();
}

static void power_update(void)
{
    if (s_screen == SCR_SETTINGS && lv_slider_is_dragged(s_set_slider)) return;   // live preview
    display_set_backlight(power_target());
}

// Something happened that the person should see: count it as activity so
// the screen comes back on.
static void ui_wake(void)
{
    s_forced_off = false;
    lv_display_trigger_activity(NULL);
    power_update();
}

static bool screen_is_dark(void)
{
    return display_get_backlight() < settings_brightness();
}

// touch.c calls this on every finger-down edge (UI task, LVGL lock held).
static bool touch_wake_hook(void)
{
    if (!screen_is_dark()) return false;
    ui_wake();
    return true;   // that touch only woke the screen
}

// ── toast ───────────────────────────────────────────────────────────────────

static void toast_delete(lv_timer_t *t)
{
    (void)t;
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    s_toast_timer = NULL;
}

static void toast_clicked(lv_event_t *e)
{
    (void)e;
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    if (s_toast) { lv_obj_delete_async(s_toast); s_toast = NULL; }   // we are inside its event
}

static void toast_show(const char *text, uint32_t accent)
{
    if (!text || !text[0]) return;
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, 176, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(accent), 0);
    lv_obj_set_style_pad_all(s_toast, 8, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -26);
    no_scroll(s_toast);
    lv_obj_add_event_cb(s_toast, toast_clicked, LV_EVENT_CLICKED, NULL);   // tap = dismiss
    lv_obj_t *l = lv_label_create(s_toast);
    lv_label_set_text(l, text);
    lv_obj_set_width(l, 156);
    lv_obj_set_height(l, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(l, 72, 0);   // four lines at most
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    s_toast_timer = lv_timer_create(toast_delete, TOAST_MS, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

// ── forward declarations ────────────────────────────────────────────────────

static void render_home(void);
static void question_open(void);
static void question_render(void);
static void question_restyle(void);
static void open_settings(void);
static void open_pad(void);
static void go_home(lv_screen_load_anim_t anim);

// ── home screen ─────────────────────────────────────────────────────────────

static void home_select(int index, bool send_focus)
{
    if (s_agent_count == 0) return;
    index %= s_agent_count;
    if (index < 0) index += s_agent_count;
    const bool moved = index != s_index;
    s_index = index;
    s_stop_armed = false;
    render_home();
    // The tile the carousel settled on — where the user is LOOKING.
    if (moved && send_focus) cable_client_send_focus(s_agents[s_index].id);
}

static void home_open_current(void)
{
    if (s_agent_count == 0) return;
    cable_client_send_open(s_agents[s_index].id, NULL);   // a person's tap: no reason
    toast_show(TR(LV_SYMBOL_UPLOAD "  Ouvert sur l'ordinateur", LV_SYMBOL_UPLOAD "  Opened on the computer"),
               COL_ACCENT);
}

static void card_clicked(lv_event_t *e)
{
    (void)e;
    if (gesture_in_progress()) return;
    home_open_current();
}

static void chev_clicked(lv_event_t *e)
{
    if (gesture_in_progress()) return;
    const int step = (int)(intptr_t)lv_event_get_user_data(e);
    home_select(s_index + step, true);
}

static void stop_disarm(lv_timer_t *t)
{
    (void)t;
    s_stop_timer = NULL;
    s_stop_armed = false;
    if (s_screen == SCR_HOME) render_home();
}

static void action_clicked(lv_event_t *e)
{
    (void)e;
    if (gesture_in_progress()) return;
    if (s_q_pending > 0) {            // the chip is the way back to a dismissed question
        question_open();
        return;
    }
    if (s_agent_count == 0 || strcmp(s_agents[s_index].state, "running") != 0) return;
    if (!s_stop_armed) {              // first tap arms, second tap stops: no accidents
        s_stop_armed = true;
        if (s_stop_timer) lv_timer_delete(s_stop_timer);
        s_stop_timer = lv_timer_create(stop_disarm, STOP_CONFIRM_MS, NULL);
        lv_timer_set_repeat_count(s_stop_timer, 1);
        render_home();
        return;
    }
    s_stop_armed = false;
    if (s_stop_timer) { lv_timer_delete(s_stop_timer); s_stop_timer = NULL; }
    cable_client_send_stop(s_agents[s_index].id);
    toast_show(TR(LV_SYMBOL_STOP "  Arrêt demandé", LV_SYMBOL_STOP "  Stop requested"), COL_ERROR);
    render_home();
}

static void home_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_LEFT:   home_select(s_index + 1, true); break;   // finger moved ←: next
    case LV_DIR_RIGHT:  home_select(s_index - 1, true); break;
    case LV_DIR_BOTTOM: open_settings(); break;                  // pull down
    case LV_DIR_TOP:    open_pad(); break;                       // push up
    default: break;
    }
}

static void pulse_tick(lv_timer_t *t)
{
    (void)t;
    s_pulse_on = !s_pulse_on;
    lv_obj_set_style_border_opa(s_ring, s_pulse_on ? LV_OPA_COVER : LV_OPA_30, 0);
}

static void busy_tick(lv_timer_t *t)
{
    (void)t;
    if (s_screen != SCR_HOME || s_agent_count == 0 ||
        strcmp(s_agents[s_index].state, "running") != 0) {
        return;
    }
    static const char *const dots[] = { "", ".", "..", "..." };
    s_busy_phase = (s_busy_phase + 1) % 4;
    lv_label_set_text_fmt(s_h_state, LV_SYMBOL_BULLET " %s%s", label_of_state("running"),
                          dots[s_busy_phase]);
}

static void build_home(void)
{
    s_scr_home = new_screen();
    lv_obj_add_event_cb(s_scr_home, home_gesture, LV_EVENT_GESTURE, NULL);

    // Status ring: a circle the size of the panel whose border carries the
    // agent's state colour.
    s_ring = new_ring(s_scr_home, RING_D, RING_W, COL_IDLE);

    s_h_header = new_label(s_scr_home, "", &font_ui_14, COL_DIM, 104, LV_ALIGN_TOP_MID, 0, 24);
    lv_label_set_long_mode(s_h_header, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_h_header, 18);
    s_h_state = new_label(s_scr_home, "", &font_ui_14, COL_IDLE, 124, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_long_mode(s_h_state, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_h_state, 18);

    // Fleet badge (agents.end.total), at 45° top-right, clear of the ring.
    s_badge = new_box(s_scr_home);
    lv_obj_set_size(s_badge, 30, 30);
    lv_obj_set_pos(s_badge, 169, 47);
    lv_obj_set_style_radius(s_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x1f6feb), 0);
    lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
    s_badge_label = lv_label_create(s_badge);
    lv_obj_center(s_badge_label);

    // The card: agent name (1–2 lines), engine · machine, then the last line
    // using whatever height is left. Tap = agent.open.
    s_card = lv_obj_create(s_scr_home);
    // 110 px = two lines of name (50) + engine line (18) + two lines of
    // status (36) + gaps: a long name never squeezes the status to one line.
    lv_obj_set_size(s_card, CARD_W, 110);
    lv_obj_align(s_card, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(COL_SURFACE2), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_card, 0, 0);
    lv_obj_set_style_radius(s_card, 16, 0);
    lv_obj_set_style_pad_all(s_card, 0, 0);
    lv_obj_set_style_pad_row(s_card, 1, 0);
    lv_obj_set_flex_flow(s_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    no_scroll(s_card);
    lv_obj_add_event_cb(s_card, card_clicked, LV_EVENT_SHORT_CLICKED, NULL);

    s_h_name = new_label(s_card, "", &font_ui_20, COL_TEXT, TEXT_W, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_long_mode(s_h_name, LV_LABEL_LONG_DOT);   // height set per text: set_name()
    s_h_sub = new_label(s_card, "", &font_ui_14, COL_DIM, TEXT_W, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_long_mode(s_h_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_h_sub, 18);
    s_h_summary = new_label(s_card, "", &font_ui_14, COL_TEXT, TEXT_W, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_long_mode(s_h_summary, LV_LABEL_LONG_DOT);   // what fits, then "…"
    lv_obj_set_flex_grow(s_h_summary, 1);

    // Carousel chevrons: tappable, and they tell a newcomer the carousel exists.
    s_chev_l = new_button(s_scr_home, "‹", 30, 60, COL_BG, COL_DIM, chev_clicked, (void *)(intptr_t)-1);
    lv_obj_set_style_bg_opa(s_chev_l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_chev_l, 0), &font_ui_28, 0);
    lv_obj_align(s_chev_l, LV_ALIGN_LEFT_MID, 14, -4);
    s_chev_r = new_button(s_scr_home, "›", 30, 60, COL_BG, COL_DIM, chev_clicked, (void *)(intptr_t)1);
    lv_obj_set_style_bg_opa(s_chev_r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_chev_r, 0), &font_ui_28, 0);
    lv_obj_align(s_chev_r, LV_ALIGN_RIGHT_MID, -14, -4);

    // Page dots.
    s_dots = new_box(s_scr_home);
    lv_obj_set_size(s_dots, 110, 8);
    lv_obj_align(s_dots, LV_ALIGN_TOP_MID, 0, 176);
    lv_obj_set_style_pad_column(s_dots, 6, 0);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < CABLE_MAX_AGENTS; i++) {
        lv_obj_t *d = new_box(s_dots);
        lv_obj_set_size(d, 6, 6);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(COL_LINE), 0);
    }

    // Action chip: "? Question" (a question waits) or "■ Stop" (turn running).
    s_action = new_button(s_scr_home, "", 96, 32, COL_SURFACE2, COL_TEXT, action_clicked, NULL);
    lv_obj_align(s_action, LV_ALIGN_TOP_MID, 0, 186);
    s_action_label = lv_obj_get_child(s_action, 0);

    s_pulse_timer = lv_timer_create(pulse_tick, PULSE_PERIOD_MS, NULL);
    lv_timer_pause(s_pulse_timer);
    s_busy_timer = lv_timer_create(busy_tick, BUSY_PERIOD_MS, NULL);
}

// The agent name gets one or two lines of font_ui_20 — then "…". Measured
// here because LVGL's LONG_DOT cannot size itself: with a content-sized
// height it measures the ALREADY shortened text and stays one line.
static void set_name(const char *text)
{
    lv_point_t size;
    lv_text_get_size(&size, text, &font_ui_20, 0, 0, TEXT_W, LV_TEXT_FLAG_NONE);
    const int32_t line = lv_font_get_line_height(&font_ui_20);
    lv_obj_set_height(s_h_name, size.y > line ? 2 * line : line);
    lv_label_set_text(s_h_name, text);
}

static void render_home(void)
{
    lv_label_set_text(s_h_header, s_machine_name);

    const bool question_waiting = s_q_pending > 0 || s_notif_questions > 0;
    uint32_t ring = COL_IDLE;

    if (s_agent_count == 0) {
        lv_label_set_text(s_h_state, "");
        set_name(TR("Aucun agent", "No agents"));
        lv_label_set_text(s_h_sub, "");
        lv_label_set_text(s_h_summary,
                          s_has_window ? TR("Rien sur cet onglet.", "Nothing on this tab.")
                                       : TR("Ouvrez Harness sur l'ordinateur.",
                                            "Open Harness on the computer."));
    } else {
        if (s_index >= s_agent_count) s_index = s_agent_count - 1;
        if (s_index < 0) s_index = 0;
        const cable_agent_t *a = &s_agents[s_index];
        ring = color_of_state(a->state);
        lv_obj_set_style_text_color(s_h_state, lv_color_hex(ring), 0);
        lv_label_set_text_fmt(s_h_state, LV_SYMBOL_BULLET " %s", label_of_state(a->state));
        set_name(a->name);
        if (a->engine[0] && a->machine[0]) {
            lv_label_set_text_fmt(s_h_sub, "%s · %s", a->engine, a->machine);
        } else {
            lv_label_set_text(s_h_sub, a->engine[0] ? a->engine : a->machine);
        }
        lv_label_set_text(s_h_summary, a->summary);
    }
    if (question_waiting) ring = COL_WAITING;
    lv_obj_set_style_border_color(s_ring, lv_color_hex(ring), 0);

    if (question_waiting) {
        lv_timer_resume(s_pulse_timer);
    } else {
        lv_timer_pause(s_pulse_timer);
        s_pulse_on = true;
        lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
    }

    lv_label_set_text_fmt(s_badge_label, "%d", s_fleet_total);
    set_hidden(s_badge, s_fleet_total <= 0);

    const bool many = s_agent_count > 1;
    set_hidden(s_chev_l, !many);
    set_hidden(s_chev_r, !many);
    set_hidden(s_dots, !many);
    for (int i = 0; i < CABLE_MAX_AGENTS; i++) {
        lv_obj_t *d = lv_obj_get_child(s_dots, i);
        set_hidden(d, i >= s_agent_count);
        lv_obj_set_style_bg_color(d, lv_color_hex(i == s_index ? COL_TEXT : COL_LINE), 0);
    }

    const bool running = s_agent_count > 0 && strcmp(s_agents[s_index].state, "running") == 0;
    if (!running) s_stop_armed = false;
    if (s_q_pending > 0) {
        lv_obj_set_style_bg_color(s_action, lv_color_hex(COL_WAITING), 0);
        lv_obj_set_style_text_color(s_action_label, lv_color_hex(COL_BG), 0);
        if (s_q_pending > 1) lv_label_set_text_fmt(s_action_label, "? Questions (%d)", s_q_pending);
        else lv_label_set_text(s_action_label, "? Question");
        set_hidden(s_action, false);
    } else if (running && s_has_touch) {
        lv_obj_set_style_bg_color(s_action, lv_color_hex(s_stop_armed ? COL_ERROR : COL_SURFACE2), 0);
        lv_obj_set_style_text_color(s_action_label, lv_color_hex(s_stop_armed ? COL_TEXT : COL_ERROR), 0);
        lv_label_set_text(s_action_label, s_stop_armed ? TR("Confirmer ?", "Confirm?")
                                                       : LV_SYMBOL_STOP "  Stop");
        set_hidden(s_action, false);
    } else {
        set_hidden(s_action, true);
    }
}

static void refresh_agents(void)
{
    char current[ID_MAX] = "";
    if (s_agent_count > 0 && s_index < s_agent_count) {
        memcpy(current, s_agents[s_index].id, sizeof(current));
    }
    s_agent_count = cable_client_list_agents(s_agents, CABLE_MAX_AGENTS);
    s_fleet_total = cable_client_agent_total();
    s_has_window = cable_client_has_window();
    // Stay on the same agent across a list refresh (the list is re-sent on
    // every tab switch and reorder).
    if (current[0]) {
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].id, current) == 0) { s_index = i; break; }
        }
    }
    if (s_index >= s_agent_count) s_index = s_agent_count > 0 ? s_agent_count - 1 : 0;
}

// ── question screen ─────────────────────────────────────────────────────────

static const cable_question_item_t *q_item(void)
{
    if (!s_q_valid || s_q_index >= s_q.count) return NULL;
    return &s_q.items[s_q_index];
}

static bool q_has_choice(void)
{
    const cable_question_item_t *it = q_item();
    if (!it) return false;
    if (!it->multi) return s_q_single >= 0 && s_q_single < it->opt_count;
    for (int i = 0; i < it->opt_count; i++) if (s_q_multi[i]) return true;
    return false;
}

static void q_reset_item(void)
{
    s_q_single = -1;
    memset(s_q_multi, 0, sizeof(s_q_multi));
    s_q_cursor = 0;
}

// Load the oldest pending question (a copy) and show it.
static void question_open(void)
{
    s_q_pending = cable_client_question_count();
    if (!cable_client_copy_question(&s_q)) {
        s_q_valid = false;
        go_home(LV_SCR_LOAD_ANIM_NONE);
        return;
    }
    s_q_valid = true;
    s_q_index = 0;
    q_reset_item();
    if (s_answers) cJSON_Delete(s_answers);
    s_answers = cJSON_CreateObject();
    question_render();
    lv_obj_scroll_to_y(s_q_col, 0, LV_ANIM_OFF);
    load(s_scr_question, SCR_QUESTION, LV_SCR_LOAD_ANIM_FADE_IN);
}

static void question_leave(void)
{
    if (s_answers) { cJSON_Delete(s_answers); s_answers = NULL; }
    s_q_valid = false;
    go_home(LV_SCR_LOAD_ANIM_NONE);
}

// ✕ / swipe / BOOT: no message (SPEC.md §6). The question stays pending on
// the daemon and in the queue; the amber chip on home brings it back.
static void question_dismiss(void) { question_leave(); }

// Everything answered: send, then show the next queued question, if any.
static void question_finish(void)
{
    if (!s_answers) s_answers = cJSON_CreateObject();
    cable_client_answer(s_q.agent_id, s_q.request_id, s_answers);
    cJSON_Delete(s_answers);
    s_answers = NULL;
    s_q_valid = false;
    s_q_pending = cable_client_question_count();
    toast_show(TR(LV_SYMBOL_OK "  Réponse envoyée", LV_SYMBOL_OK "  Answer sent"), COL_DONE);
    if (s_q_pending > 0) question_open();
    else go_home(LV_SCR_LOAD_ANIM_NONE);
}

// Record the current item's answer and move on.
static void question_submit(void)
{
    const cable_question_item_t *it = q_item();
    if (!it || !s_answers || !q_has_choice()) return;
    if (it->multi) {
        // Labels joined with ", " (PROTOCOL.md §4.14), in option order.
        char joined[CABLE_OPT_MAX * (CABLE_OPT_TEXT_MAX + 2)];
        joined[0] = '\0';
        for (int i = 0; i < it->opt_count; i++) {
            if (!s_q_multi[i]) continue;
            if (joined[0]) strncat(joined, ", ", sizeof(joined) - strlen(joined) - 1);
            strncat(joined, it->options[i], sizeof(joined) - strlen(joined) - 1);
        }
        cJSON_AddStringToObject(s_answers, it->key, joined);
    } else {
        cJSON_AddStringToObject(s_answers, it->key, it->options[s_q_single]);
    }
    if (s_q_index + 1 < s_q.count) {
        s_q_index++;
        q_reset_item();
        question_render();
        lv_obj_scroll_to_y(s_q_col, 0, LV_ANIM_OFF);
    } else {
        question_finish();
    }
}

static void q_option_clicked(lv_event_t *e)
{
    if (gesture_in_progress()) return;
    const cable_question_item_t *it = q_item();
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!it || i < 0 || i >= it->opt_count) return;
    if (it->multi) s_q_multi[i] = !s_q_multi[i];
    else s_q_single = (s_q_single == i) ? -1 : i;
    s_q_cursor = i;
    question_restyle();
    // On a round screen the ✓ row usually sits below the edge: once there is
    // something to send, bring it into view (a single choice is final, so
    // it goes there straight away; a multi-select only after the first pick).
    if (q_has_choice()) lv_obj_scroll_to_view(lv_obj_get_parent(s_q_send), LV_ANIM_ON);
}

static void q_send_clicked(lv_event_t *e)
{
    (void)e;
    if (gesture_in_progress()) return;
    question_submit();
}

static void q_close_clicked(lv_event_t *e)
{
    (void)e;
    if (gesture_in_progress()) return;
    question_dismiss();
}

static void question_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    const lv_dir_t d = lv_indev_get_gesture_dir(indev);
    if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) question_dismiss();   // swipe away = later
}

static void build_question(void)
{
    s_scr_question = new_screen();
    lv_obj_add_event_cb(s_scr_question, question_gesture, LV_EVENT_GESTURE, NULL);

    // One vertically scrolling column: on a round panel the content scrolls
    // THROUGH the wide middle; the top/bottom padding lets the first and last
    // rows reach it.
    s_q_col = lv_obj_create(s_scr_question);
    lv_obj_set_size(s_q_col, 200, SCR_W);
    lv_obj_center(s_q_col);
    lv_obj_set_style_bg_opa(s_q_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_q_col, 0, 0);
    lv_obj_set_style_pad_top(s_q_col, 30, 0);
    lv_obj_set_style_pad_bottom(s_q_col, 44, 0);
    lv_obj_set_style_pad_hor(s_q_col, 4, 0);
    lv_obj_set_style_pad_row(s_q_col, 8, 0);
    lv_obj_set_flex_flow(s_q_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_q_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_q_col, LV_DIR_VER);   // sideways strokes stay gestures
    lv_obj_set_scrollbar_mode(s_q_col, LV_SCROLLBAR_MODE_OFF);

    s_q_header = lv_label_create(s_q_col);
    lv_obj_set_size(s_q_header, 150, 18);
    lv_obj_set_style_text_align(s_q_header, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_q_header, lv_color_hex(COL_WAITING), 0);
    lv_label_set_long_mode(s_q_header, LV_LABEL_LONG_DOT);

    s_q_text = lv_label_create(s_q_col);
    lv_obj_set_width(s_q_text, 176);
    lv_obj_set_style_text_align(s_q_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_q_text, LV_LABEL_LONG_WRAP);

    // Options live in their own column so they can be rebuilt per item.
    s_q_opts = new_box(s_q_col);
    lv_obj_set_size(s_q_opts, 184, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_row(s_q_opts, 6, 0);
    lv_obj_set_flex_flow(s_q_opts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_q_opts, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *row = new_box(s_q_col);
    lv_obj_set_size(row, 150, 48);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_q_close = new_button(row, LV_SYMBOL_CLOSE, 56, 42, COL_SURFACE2, COL_DIM, q_close_clicked, NULL);
    s_q_send = new_button(row, LV_SYMBOL_OK, 72, 42, COL_DONE, COL_BG, q_send_clicked, NULL);
}

static void style_option(lv_obj_t *b, lv_obj_t *label, const cable_question_item_t *it, int i,
                         bool cursor)
{
    const bool chosen = it->multi ? s_q_multi[i] : s_q_single == i;
    lv_obj_set_style_bg_color(b, lv_color_hex(chosen ? 0x16324f : COL_SURFACE), 0);
    lv_obj_set_style_border_width(b, chosen || cursor ? 2 : 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(chosen ? COL_RUNNING : cursor ? COL_DIM : COL_LINE), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(chosen ? COL_TEXT : 0xc0c8d2), 0);
    if (it->multi) {
        lv_label_set_text_fmt(label, "%s %s", chosen ? LV_SYMBOL_OK : LV_SYMBOL_BULLET, it->options[i]);
    } else {
        lv_label_set_text(label, it->options[i]);
    }
}

// Selection, ✓ state and the button cursor — WITHOUT rebuilding anything:
// this runs inside an option's own click handler, and deleting the object
// whose event is being dispatched is how LVGL UIs crash.
static void question_restyle(void)
{
    const cable_question_item_t *it = q_item();
    if (!it) return;
    const int n = it->opt_count;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_get_child(s_q_opts, i);
        if (b) style_option(b, lv_obj_get_child(b, 0), it, i, !s_has_touch && s_q_cursor == i);
    }
    if (n > 0) {
        if (q_has_choice()) lv_obj_remove_state(s_q_send, LV_STATE_DISABLED);
        else lv_obj_add_state(s_q_send, LV_STATE_DISABLED);
    }
    // Button navigation: the cursor walks the options, then ✓, then ✕.
    if (!s_has_touch) {
        lv_obj_set_style_border_width(s_q_send, s_q_cursor == n ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_q_send, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_border_width(s_q_close, s_q_cursor == n + 1 ? 2 : 0, 0);
        lv_obj_set_style_border_color(s_q_close, lv_color_hex(COL_TEXT), 0);
        lv_obj_t *target = s_q_cursor < n ? lv_obj_get_child(s_q_opts, s_q_cursor)
                                          : lv_obj_get_parent(s_q_send);
        if (target) lv_obj_scroll_to_view(target, LV_ANIM_ON);
    }
}

// Build the current item (header, prompt, one button per option), then style.
static void question_render(void)
{
    const cable_question_item_t *it = q_item();
    if (!it) return;

    const char *who = s_q.name[0] ? s_q.name : "Agent";
    const int others = s_q_pending > 1 ? s_q_pending - 1 : 0;
    if (s_q.count > 1 && others) {
        lv_label_set_text_fmt(s_q_header, "%s · %d/%d · +%d", who, s_q_index + 1, s_q.count, others);
    } else if (s_q.count > 1) {
        lv_label_set_text_fmt(s_q_header, "%s · %d/%d", who, s_q_index + 1, s_q.count);
    } else if (others) {
        lv_label_set_text_fmt(s_q_header, "%s · +%d", who, others);
    } else {
        lv_label_set_text(s_q_header, who);
    }
    lv_label_set_text(s_q_text, it->q);

    lv_obj_clean(s_q_opts);
    for (int i = 0; i < it->opt_count; i++) {
        lv_obj_t *b = lv_button_create(s_q_opts);
        lv_obj_set_width(b, 184);
        lv_obj_set_height(b, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(b, 40, 0);   // finger-sized
        lv_obj_set_style_radius(b, 20, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_ver(b, 8, 0);
        lv_obj_set_style_pad_hor(b, 14, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COL_LINE), LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, q_option_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }

    if (it->opt_count == 0) {
        // Free-text question: this dial has no keyboard and no microphone.
        lv_obj_t *l = lv_label_create(s_q_opts);
        lv_obj_set_width(l, 176);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(COL_DIM), 0);
        lv_label_set_text(l, TR("Réponse libre : répondez sur l'ordinateur.",
                                "Free-text answer: reply on the computer."));
        set_hidden(s_q_send, true);
    } else {
        set_hidden(s_q_send, false);
        // → when another item of the same request follows, ✓ on the last.
        lv_label_set_text(lv_obj_get_child(s_q_send, 0),
                          s_q_index + 1 < s_q.count ? LV_SYMBOL_RIGHT : LV_SYMBOL_OK);
    }
    question_restyle();
}

// Re-read the queue: the question on screen may have been answered or
// closed elsewhere, and new ones may have arrived behind it.
static void questions_changed(void)
{
    s_q_pending = cable_client_question_count();
    if (s_screen != SCR_QUESTION || !s_q_valid) return;
    char head[ID_MAX];
    cable_client_question_head_id(head, sizeof(head));
    if (!head[0]) {
        toast_show(TR("Question traitée ailleurs", "Answered elsewhere"), COL_DIM);
        question_leave();
        return;
    }
    // New questions only ever queue BEHIND the head, so a different head
    // means ours left the queue (question.close): move on to the next one.
    if (strcmp(head, s_q.request_id) != 0) {
        toast_show(TR("Question traitée ailleurs", "Answered elsewhere"), COL_DIM);
        question_open();
        return;
    }
    question_render();   // only the "+n" counter moved
}

// ── settings screen (pull down from home / offline) ─────────────────────────

static void settings_refresh_info(void)
{
    uint32_t corrupt = 0, discarded = 0, bad = 0, unknown = 0;
    cable_link_counters(&corrupt, &discarded);
    cable_client_counters(&bad, &unknown);
    const char *chip = touch_chip_name();
    // Three short lines: the bottom of a round screen is narrow.
    lv_label_set_text_fmt(s_set_info, "v%s · %s\nRAM %u KB\n%s %lu/%lu/%lu",
                          s_fw, chip ? chip : TR("sans tactile", "no touch"),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                          TR("Erreurs", "Errors"), (unsigned long)corrupt, (unsigned long)bad,
                          (unsigned long)unknown);
}

static void slider_changed(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    const int v = (int)lv_slider_get_value(s);
    lv_label_set_text_fmt(s_set_value, "%d %%", v);
    display_set_backlight(v);                                    // live preview
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_set_brightness(v);   // saved on release
}

static void back_home_clicked(lv_event_t *e)
{
    (void)e;
    if (gesture_in_progress()) return;
    go_home(s_screen == SCR_SETTINGS ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
}

static void settings_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) go_home(LV_SCR_LOAD_ANIM_MOVE_TOP);
}

// Touch check: a dot follows the finger anywhere on the settings screen. If
// it moves the wrong way, fix Touch → swap / mirror in menuconfig (README).
static void settings_touch(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    if (code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_obj_set_pos(s_set_dot, p.x - 7, p.y - 7);
        set_hidden(s_set_dot, false);
    } else {
        set_hidden(s_set_dot, true);
    }
}

static void build_settings(void)
{
    s_scr_settings = new_screen();
    lv_obj_add_event_cb(s_scr_settings, settings_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_scr_settings, settings_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr_settings, settings_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr_settings, settings_touch, LV_EVENT_PRESS_LOST, NULL);
    new_ring(s_scr_settings, RING_D, 2, COL_LINE);

    new_label(s_scr_settings, TR("Réglages", "Settings"), &font_ui_20, COL_TEXT, 150,
              LV_ALIGN_TOP_MID, 0, 26);
    new_label(s_scr_settings, TR("Luminosité", "Brightness"), &font_ui_14, COL_DIM, 150,
              LV_ALIGN_TOP_MID, 0, 62);
    s_set_value = new_label(s_scr_settings, "", &font_ui_14, COL_TEXT, 60, LV_ALIGN_TOP_MID, 0, 82);

    s_set_slider = lv_slider_create(s_scr_settings);
    lv_obj_set_size(s_set_slider, 160, 14);
    lv_obj_align(s_set_slider, LV_ALIGN_TOP_MID, 0, 110);
    lv_slider_set_range(s_set_slider, 5, 100);
    lv_obj_set_ext_click_area(s_set_slider, 16);   // easy to grab with a thumb
    lv_obj_set_style_bg_color(s_set_slider, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_set_slider, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_set_slider, lv_color_hex(COL_TEXT), LV_PART_KNOB);
    // Dragging the knob sideways must not read as a swipe on the screen.
    lv_obj_remove_flag(s_set_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_set_slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_set_slider, slider_changed, LV_EVENT_RELEASED, NULL);

    s_set_info = new_label(s_scr_settings, "", &font_ui_14, COL_DIM, 176, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_t *b = new_button(s_scr_settings, LV_SYMBOL_UP, 56, 32, COL_SURFACE2, COL_TEXT,
                             back_home_clicked, NULL);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -14);

    s_set_dot = new_box(s_scr_settings);
    lv_obj_set_size(s_set_dot, 14, 14);
    lv_obj_set_style_radius(s_set_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_set_dot, lv_color_hex(COL_WAITING), 0);
    lv_obj_set_style_bg_opa(s_set_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_set_dot, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
}

static void open_settings(void)
{
    lv_slider_set_value(s_set_slider, settings_brightness(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_set_value, "%d %%", settings_brightness());
    settings_refresh_info();
    load(s_scr_settings, SCR_SETTINGS, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
}

// ── scrollpad (push up from home): the dial scrolls the computer's window ──
// PROTOCOL.md §4.13: report MOVEMENT (dy since the last report, + = down the
// glass): "down" on landing (stops a fling), "move" while travelling, "up" on
// lift with the velocity that becomes the fling. A zero dy is never elided.

#define VEL_SAMPLES 6
static struct {
    bool    active;
    int32_t last_sent_y;
    int64_t last_send_us;
    int64_t t[VEL_SAMPLES];
    int32_t y[VEL_SAMPLES];
    int     n, head;
} s_pad;

static void pad_sample(int64_t now, int32_t y)
{
    s_pad.t[s_pad.head] = now;
    s_pad.y[s_pad.head] = y;
    s_pad.head = (s_pad.head + 1) % VEL_SAMPLES;
    if (s_pad.n < VEL_SAMPLES) s_pad.n++;
}

// Velocity over the last ~100 ms, px/s; 0 when the finger had come to rest
// before lifting, so the lift lands as a stop, not a throw.
static int pad_velocity(int64_t now)
{
    if (s_pad.n < 2) return 0;
    const int newest = (s_pad.head + VEL_SAMPLES - 1) % VEL_SAMPLES;
    int oldest = newest;
    for (int k = 1; k < s_pad.n; k++) {
        const int i = (newest + VEL_SAMPLES - k) % VEL_SAMPLES;
        if (s_pad.t[newest] - s_pad.t[i] > 100000) break;
        oldest = i;
    }
    // The finger must have MOVED recently: compare against the last sample
    // that differs from the lift point.
    int moved_at = -1;
    for (int k = 1; k < s_pad.n; k++) {
        const int i = (newest + VEL_SAMPLES - k) % VEL_SAMPLES;
        if (s_pad.y[i] != s_pad.y[newest]) { moved_at = i; break; }
    }
    if (moved_at < 0 || now - s_pad.t[moved_at] > 80000) return 0;
    const int64_t dt = s_pad.t[newest] - s_pad.t[oldest];
    if (dt <= 0) return 0;
    int64_t v = (int64_t)(s_pad.y[newest] - s_pad.y[oldest]) * 1000000 / dt;
    if (v > 8000) v = 8000;
    if (v < -8000) v = -8000;
    return (int)v;
}

static void pad_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int64_t now = esp_timer_get_time();

    if (code == LV_EVENT_PRESSED) {
        s_pad.active = true;
        s_pad.last_sent_y = p.y;
        s_pad.last_send_us = now;
        s_pad.n = s_pad.head = 0;
        pad_sample(now, p.y);
        cable_client_send_scroll("down", 0, 0, false);
        lv_obj_set_style_border_color(s_pad_ring, lv_color_hex(COL_ACCENT), 0);
    } else if (code == LV_EVENT_PRESSING && s_pad.active) {
        pad_sample(now, p.y);
        const int dy = p.y - s_pad.last_sent_y;
        if (dy != 0 && now - s_pad.last_send_us >= SCROLL_SEND_MS * 1000) {
            cable_client_send_scroll("move", dy, 0, false);
            s_pad.last_sent_y = p.y;
            s_pad.last_send_us = now;
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_pad.active) {
        s_pad.active = false;
        cable_client_send_scroll("up", p.y - s_pad.last_sent_y, pad_velocity(now), true);
        lv_obj_set_style_border_color(s_pad_ring, lv_color_hex(COL_LINE), 0);
    }
}

static void pad_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    const lv_dir_t d = lv_indev_get_gesture_dir(indev);
    // Vertical strokes ARE the scrolling; a sideways swipe leaves.
    if (d == LV_DIR_LEFT || d == LV_DIR_RIGHT) go_home(LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
}

static void build_pad(void)
{
    s_scr_pad = new_screen();
    lv_obj_add_event_cb(s_scr_pad, pad_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr_pad, pad_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr_pad, pad_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr_pad, pad_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_scr_pad, pad_gesture, LV_EVENT_GESTURE, NULL);

    s_pad_ring = new_ring(s_scr_pad, 200, 2, COL_LINE);
    new_label(s_scr_pad, LV_SYMBOL_UP, &font_ui_28, COL_DIM, 0, LV_ALIGN_TOP_MID, 0, 34);
    new_label(s_scr_pad, TR("Défilement", "Scroll"), &font_ui_20, COL_TEXT, 150, LV_ALIGN_CENTER, 0, -34);
    new_label(s_scr_pad, TR("Glissez ↑↓ : la fenêtre\ndéfile sur l'ordinateur",
                            "Drag ↑↓ to scroll the\nwindow on the computer"),
              &font_ui_14, COL_DIM, 170, LV_ALIGN_CENTER, 0, 4);
    new_label(s_scr_pad, LV_SYMBOL_DOWN, &font_ui_28, COL_DIM, 0, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_t *b = new_button(s_scr_pad, LV_SYMBOL_CLOSE, 44, 44, COL_SURFACE2, COL_TEXT,
                             back_home_clicked, NULL);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -14);
}

static void open_pad(void)
{
    if (!s_connected) return;
    memset(&s_pad, 0, sizeof(s_pad));
    load(s_scr_pad, SCR_PAD, LV_SCR_LOAD_ANIM_MOVE_TOP);
}

// ── boot / offline ──────────────────────────────────────────────────────────

static void build_boot(void)
{
    s_scr_boot = new_screen();
    new_label(s_scr_boot, "λ", &font_logo_72, COL_ACCENT, 0, LV_ALIGN_CENTER, 0, -40);
    new_label(s_scr_boot, "Harness C3", &font_ui_20, COL_TEXT, 170, LV_ALIGN_CENTER, 0, 24);
    lv_obj_t *v = new_label(s_scr_boot, "", &font_ui_14, COL_DIM, 170, LV_ALIGN_CENTER, 0, 52);
    lv_label_set_text_fmt(v, "v%s", s_fw);
}

static void offline_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM) open_settings();
}

static void build_offline(void)
{
    s_scr_offline = new_screen();
    lv_obj_add_event_cb(s_scr_offline, offline_gesture, LV_EVENT_GESTURE, NULL);
    new_ring(s_scr_offline, RING_D, 2, COL_LINE);
    new_label(s_scr_offline, LV_SYMBOL_USB, &font_ui_28, COL_DIM, 0, LV_ALIGN_CENTER, 0, -60);
    new_label(s_scr_offline, TR("Non connecté", "Not connected"), &font_ui_20, COL_TEXT, 170,
              LV_ALIGN_CENTER, 0, -20);
    new_label(s_scr_offline,
              TR("Branchez sur un PC\nqui fait tourner Harness",
                 "Plug me into a computer\nrunning Harness"),
              &font_ui_14, COL_DIM, 176, LV_ALIGN_CENTER, 0, 20);
    new_label(s_scr_offline, TR("câble USB-A → USB-C", "USB-A → USB-C cable"), &font_ui_14,
              COL_IDLE, 170, LV_ALIGN_CENTER, 0, 60);
}

static void go_home(lv_screen_load_anim_t anim)
{
    if (!s_connected) {
        load(s_scr_offline, SCR_OFFLINE, anim);
        return;
    }
    s_q_pending = cable_client_question_count();
    render_home();
    load(s_scr_home, SCR_HOME, anim);
}

// ── cable_client event sinks (LINK READER TASK — flag / queue and return) ──

static void mark(unsigned bits) { atomic_fetch_or(&s_dirty, bits); }

static void push_ev(const ui_ev_t *ev)
{
    if (s_ev_queue && xQueueSend(s_ev_queue, ev, 0) != pdTRUE) {
        ESP_LOGD(TAG, "ui event %d dropped (queue full)", (int)ev->type);
    }
}

static void on_session(bool up, const char *machine_name, void *ctx)
{
    (void)ctx;
    ui_ev_t ev = { .type = UI_EV_SESSION };
    ev.d.session.up = up;
    cable_utf8_copy(ev.d.session.name, sizeof(ev.d.session.name), machine_name);
    push_ev(&ev);
    mark(DIRTY_SESSION);
}

static void on_agents_changed(void *ctx) { (void)ctx; mark(DIRTY_AGENTS); }

static void on_agent_event(const char *agent_id, const char *state, const char *text,
                           bool notify, bool beep, void *ctx)
{
    (void)ctx;
    (void)state;
    mark(DIRTY_AGENTS);
    if (!notify && !beep) return;   // plain state moves ride on the dirty bit
    ui_ev_t ev = { .type = UI_EV_AGENT_EVENT };
    cable_utf8_copy(ev.d.agent.id, sizeof(ev.d.agent.id), agent_id);
    cable_utf8_copy(ev.d.agent.text, sizeof(ev.d.agent.text), text);
    ev.d.agent.notify = notify;
    ev.d.agent.beep = beep;
    push_ev(&ev);
}

static void on_notif(const cable_notif_t *items, int count, void *ctx)
{
    (void)items; (void)count; (void)ctx;
    mark(DIRTY_NOTIF);   // the UI re-reads the whole list under the client lock
}

static void on_question(const cable_question_t *q, void *ctx)
{
    (void)q; (void)ctx;
    mark(DIRTY_QUESTIONS);
    push_ev(&(ui_ev_t){ .type = UI_EV_QUESTION });
}

static void on_question_closed(void *ctx) { (void)ctx; mark(DIRTY_QUESTIONS); }

static void on_toast(const char *text, void *ctx)
{
    (void)ctx;
    ui_ev_t ev = { .type = UI_EV_TOAST };
    cable_utf8_copy(ev.d.toast.text, sizeof(ev.d.toast.text), text);
    push_ev(&ev);
}

static void on_focus(const char *agent_id, void *ctx)
{
    (void)ctx;
    ui_ev_t ev = { .type = UI_EV_FOCUS };
    cable_utf8_copy(ev.d.focus.id, sizeof(ev.d.focus.id), agent_id);
    push_ev(&ev);
}

// ── applying state (UI TASK ONLY) ───────────────────────────────────────────

static void apply_dirty(void)
{
    const unsigned d = atomic_exchange(&s_dirty, 0);
    if (!d) return;

    if (d & DIRTY_SESSION) {
        const bool up = cable_client_is_connected();
        // Only a TRANSITION moves the screen: `welcome` answers every 15 s
        // keepalive `hello`, and reacting to each one yanked the person off
        // the question they were answering.
        if (up != s_connected) {
            s_connected = up;
            if (up) {
                ESP_LOGI(TAG, "connected to %s", s_machine_name);
                if (s_screen == SCR_BOOT || s_screen == SCR_OFFLINE) go_home(LV_SCR_LOAD_ANIM_FADE_IN);
            } else {
                s_agent_count = 0;
                s_fleet_total = 0;
                s_notif_questions = 0;
                s_q_pending = 0;
                s_q_valid = false;
                s_stop_armed = false;
                if (s_answers) { cJSON_Delete(s_answers); s_answers = NULL; }
                if (s_screen != SCR_BOOT && s_screen != SCR_SETTINGS) {
                    load(s_scr_offline, SCR_OFFLINE, LV_SCR_LOAD_ANIM_FADE_IN);
                }
            }
        }
    }
    if (d & DIRTY_AGENTS) refresh_agents();
    if (d & DIRTY_NOTIF) s_notif_questions = cable_client_notif_question_count();
    if (d & DIRTY_QUESTIONS) questions_changed();
    if (s_screen == SCR_HOME) render_home();
}

static void apply_ev(const ui_ev_t *ev)
{
    switch (ev->type) {
    case UI_EV_SESSION:
        if (ev->d.session.up) {
            cable_utf8_copy(s_machine_name, sizeof(s_machine_name), ev->d.session.name);
            if (s_screen == SCR_HOME) lv_label_set_text(s_h_header, s_machine_name);
        }
        break;
    case UI_EV_AGENT_EVENT: {
        if (ev->d.agent.beep) buzzer_beep(BUZZER_BEEP_DONE);
        if (!ev->d.agent.notify) break;
        // A finished turn: wake the screen and show its recap — this board
        // has no buzzer, so the toast IS the notification.
        ui_wake();
        const char *name = ev->d.agent.id;
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].id, ev->d.agent.id) == 0) { name = s_agents[i].name; break; }
        }
        char msg[ID_MAX + 100];
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK " %s\n%s", name, ev->d.agent.text);
        toast_show(msg, COL_DONE);
        break;
    }
    case UI_EV_QUESTION:
        buzzer_beep(BUZZER_BEEP_QUESTION);
        ui_wake();
        s_q_pending = cable_client_question_count();
        // A question is a job: it takes the face — unless one is already
        // being answered, in which case the header's "+n" says there is more.
        if (s_screen != SCR_QUESTION) question_open();
        else question_render();
        break;
    case UI_EV_TOAST:
        buzzer_beep(BUZZER_BEEP_ERROR);
        toast_show(ev->d.toast.text, COL_ERROR);
        break;
    case UI_EV_FOCUS:
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].id, ev->d.focus.id) == 0) {
                if (i != s_index) home_select(i, false);   // the daemon moved: follow, don't echo
                break;
            }
        }
        break;
    }
}

// ── buttons (UI TASK ONLY) ──────────────────────────────────────────────────

static void handle_button(btn_event_t ev)
{
    if (ev == BTN_EVENT_B_LONG) {                 // screen off / on
        if (s_forced_off || screen_is_dark()) ui_wake();
        else { s_forced_off = true; power_update(); }
        return;
    }
    if (screen_is_dark()) {                       // a press on a dark screen only wakes it
        ui_wake();
        return;
    }
    lv_display_trigger_activity(NULL);

    switch (s_screen) {
    case SCR_HOME:
        if (ev == BTN_EVENT_A_SHORT) home_select(s_index + 1, true);
        else if (ev == BTN_EVENT_A_LONG) home_open_current();
        else if (ev == BTN_EVENT_B_SHORT && s_q_pending > 0) question_open();
        break;
    case SCR_QUESTION: {
        const cable_question_item_t *it = q_item();
        if (!it) break;
        const int n = it->opt_count;
        if (ev == BTN_EVENT_A_SHORT) {
            s_q_cursor = (s_q_cursor + 1) % (n + 2);          // options, ✓, ✕
            question_restyle();
        } else if (ev == BTN_EVENT_A_LONG) {
            if (s_q_cursor < n) {
                if (it->multi) {
                    s_q_multi[s_q_cursor] = !s_q_multi[s_q_cursor];
                    question_restyle();
                } else {
                    s_q_single = s_q_cursor;                  // one long press answers
                    question_submit();
                }
            } else if (s_q_cursor == n) {
                question_submit();
            } else {
                question_dismiss();
            }
        } else if (ev == BTN_EVENT_B_SHORT) {
            question_dismiss();
        }
        break;
    }
    case SCR_SETTINGS:
        if (ev == BTN_EVENT_B_SHORT) go_home(LV_SCR_LOAD_ANIM_MOVE_TOP);
        break;
    case SCR_PAD:
        if (ev == BTN_EVENT_B_SHORT) go_home(LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
        break;
    case SCR_OFFLINE:
    case SCR_BOOT:
    default:
        break;
    }
}

// ── boot timer, heartbeat ───────────────────────────────────────────────────

static void boot_timeout(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (s_screen == SCR_BOOT) go_home(LV_SCR_LOAD_ANIM_FADE_IN);
}

// PROTOCOL.md §9: a periodic "alive" line; the daemon marks gaps ≥ 90 s.
static void heartbeat(void)
{
    const int64_t now = esp_timer_get_time();
    if (now - s_last_alive_us < (int64_t)ALIVE_EVERY_MS * 1000) return;
    s_last_alive_us = now;
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "alive up=%llus heap=%u min=%u lvgl=%u%% frag=%u%% screen=%d bl=%d",
             (unsigned long long)(now / 1000000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)mon.used_pct, (unsigned)mon.frag_pct, (int)s_screen,
             display_get_backlight());
}

// ── UI task ─────────────────────────────────────────────────────────────────

// One pass of the UI loop. Also driven directly by the host simulator
// (test/sim), which is how every screen is exercised without a board.
static void ui_step(void)
{
    if (!display_lock(100)) return;
    apply_dirty();
    ui_ev_t ev;
    while (xQueueReceive(s_ev_queue, &ev, 0) == pdTRUE) apply_ev(&ev);
    apply_dirty();   // events above may have raised more
    btn_event_t btn;
    while (xQueueReceive(s_btn_queue, &btn, 0) == pdTRUE) handle_button(btn);
    lv_timer_handler();
    power_update();
    heartbeat();
    display_unlock();
}

#ifdef HARNESS_UI_SIM
void ui_sim_step(void) { ui_step(); }
#endif

static void ui_task(void *arg)
{
    (void)arg;
    // A wedged UI must reboot the dial rather than freeze its face
    // (CONFIG_ESP_TASK_WDT_PANIC).
    const bool wdt = esp_task_wdt_add(NULL) == ESP_OK;
    s_last_alive_us = esp_timer_get_time();
    for (;;) {
        if (wdt) esp_task_wdt_reset();
        ui_step();
        vTaskDelay(pdMS_TO_TICKS(UI_LOOP_MS));
    }
}

// ── init ────────────────────────────────────────────────────────────────────

QueueHandle_t ui_button_queue(void) { return s_btn_queue; }

bool ui_init(const char *fw_version, bool has_touch)
{
    s_has_touch = has_touch;
    cable_utf8_copy(s_fw, sizeof(s_fw), fw_version ? fw_version : "?");
    s_btn_queue = xQueueCreate(8, sizeof(btn_event_t));
    s_ev_queue = xQueueCreate(12, sizeof(ui_ev_t));
    if (!s_btn_queue || !s_ev_queue) {
        ESP_LOGE(TAG, "no memory for UI queues");
        return false;
    }

    if (!display_lock(1000)) return false;
    lv_obj_set_style_text_font(lv_layer_top(), &font_ui_14, 0);
    lv_obj_set_style_text_color(lv_layer_top(), lv_color_hex(COL_TEXT), 0);
    build_boot();
    build_offline();
    build_home();
    build_question();
    build_settings();
    build_pad();
    load(s_scr_boot, SCR_BOOT, LV_SCR_LOAD_ANIM_NONE);
    lv_timer_create(boot_timeout, BOOT_SCREEN_MS, NULL);
    // Draw the first frame NOW, then light the panel: its RAM held noise.
    lv_refr_now(NULL);
    display_set_backlight(settings_brightness());
    touch_set_wake_hook(touch_wake_hook);
    display_unlock();

    cable_client_set_ui(&(cable_client_ui_t){
        .session = on_session,
        .agents_changed = on_agents_changed,
        .agent_event = on_agent_event,
        .notif = on_notif,
        .question = on_question,
        .question_closed = on_question_closed,
        .toast = on_toast,
        .focus = on_focus,
    });

    // 8 KB: LVGL's software renderer + cJSON (answers are built here).
    if (xTaskCreate(ui_task, "ui", 8192, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
        return false;
    }
    ESP_LOGI(TAG, "ui up (fw %s, %s)", s_fw, has_touch ? "touch" : "buttons only");
    return true;
}
