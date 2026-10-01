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

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
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
#define BUSY_PERIOD_MS   1000    // turn timer "1:23": a one-line redraw
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
        struct { char id[ID_MAX]; char text[96]; char state[16]; bool notify, beep; } agent;
        struct { char text[112]; } toast;
        struct { char id[ID_MAX]; } focus;
    } d;
} ui_ev_t;

static QueueHandle_t s_btn_queue;
static QueueHandle_t s_ev_queue;

// ── UI state (UI task only) ─────────────────────────────────────────────────

typedef enum { SCR_BOOT, SCR_OFFLINE, SCR_HOME, SCR_QUESTION, SCR_SETTINGS, SCR_PAD, SCR_LAMP } screen_t;

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
                *s_scr_pad, *s_scr_lamp;

// Lamp mode: the dial used as a light (level + tone kept in NVS).
static lv_obj_t     *s_lamp_hint, *s_lamp_dot;
static lv_grad_dsc_t s_lamp_grad;          // must outlive the style that points at it
static int           s_lamp_level = 70, s_lamp_warmth = 1;
static bool          s_lamp_resume;        // a question took the face from the lamp: give it back
static struct { lv_point_t start; int level0, warm0; char axis; } s_lamp_drag;

// home
static lv_obj_t   *s_ring, *s_h_header, *s_h_state, *s_badge, *s_badge_label;
static lv_obj_t   *s_unseen_dot;      // green/red flag: work this person has not looked at
static lv_obj_t   *s_card, *s_h_name, *s_h_summary;
static lv_obj_t   *s_chev_l, *s_chev_r, *s_dots, *s_action, *s_action_label;
static lv_timer_t *s_pulse_timer, *s_busy_timer, *s_stop_timer, *s_comet_timer, *s_radar_timer;
static lv_obj_t   *s_ring_track, *s_comet, *s_comet_glow, *s_h_subrow, *s_h_engine, *s_h_machine;
static lv_obj_t   *s_boot_arc, *s_boot_logo, *s_radar, *s_pad_puck;
static int32_t     s_comet_angle, s_radar_angle;
static int         s_bl_now = -1;          // backlight as currently driven (fades toward target)

// When each running agent started its turn (UI clock), for the "1:23" timer.
static struct { char id[ID_MAX]; uint32_t since; } s_run[CABLE_MAX_AGENTS];
static bool        s_pulse_on = true;
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


// ── effects ─────────────────────────────────────────────────────────────────
// Every effect below is built to repaint as LITTLE as possible: this panel is
// fed over SPI by a single 160 MHz core that also runs the USB link. Arcs
// move by small segments (lv_arc invalidates only the changed span), text
// fades through the inherited text_opa (no off-screen layer, no LVGL-pool
// buffer), and the full-ring changes happen a few times per second at most.

// A ring drawn as an arc: MAIN = the track, INDICATOR = the coloured part.
static lv_obj_t *new_arc(lv_obj_t *parent, int diameter, int width, uint32_t color,
                         lv_opa_t track_opa)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, diameter, diameter);
    lv_obj_center(a);
    lv_arc_set_rotation(a, 270);              // 0° = 12 o'clock
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_angles(a, 0, 360);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, track_opa, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    return a;
}

static void anim_text_opa(void *o, int32_t v) { lv_obj_set_style_text_opa(o, (lv_opa_t)v, 0); }
static void anim_translate_x(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }
static void anim_translate_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void anim_arc_end(void *o, int32_t v) { lv_arc_set_end_angle(o, v); }

static void fx_start(lv_obj_t *o, lv_anim_exec_xcb_t exec, int32_t from, int32_t to,
                     uint32_t ms, uint32_t delay, lv_anim_path_cb_t path)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, path ? path : lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Content arriving: rises a few pixels while its text fades in.
static void fx_rise(lv_obj_t *o, uint32_t delay)
{
    lv_obj_set_style_text_opa(o, LV_OPA_TRANSP, 0);
    fx_start(o, anim_translate_y, 12, 0, 220, delay, NULL);
    fx_start(o, anim_text_opa, 0, 255, 220, delay, NULL);
}

// Carousel: the card slides in from the side the finger came from.
static void fx_slide_card(int dir)
{
    lv_anim_delete(s_card, anim_translate_x);
    lv_anim_delete(s_card, anim_text_opa);
    fx_start(s_card, anim_translate_x, dir * 36, 0, 230, 0, NULL);
    fx_start(s_card, anim_text_opa, 40, 255, 230, 0, NULL);
}

// A turn failed: the card shakes, briefly, like a head saying "no".
static void anim_shake(void *o, int32_t v)
{
    const float t = (float)v / 1000.0f;
    lv_obj_set_style_translate_x(o, (int32_t)(7.0f * sinf(t * 6.0f * 3.14159f) * (1.0f - t)), 0);
}

static void fx_shake(lv_obj_t *o)
{
    lv_anim_delete(o, anim_shake);
    fx_start(o, anim_shake, 0, 1000, 420, 0, lv_anim_path_linear);
}

// A turn finished: the ring fills around the dial in green.
static void fx_ring_sweep(uint32_t color)
{
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(color), LV_PART_INDICATOR);
    lv_arc_set_angles(s_ring, 0, 0);
    lv_anim_delete(s_ring, anim_arc_end);
    fx_start(s_ring, anim_arc_end, 0, 360, 650, 0, lv_anim_path_ease_in_out);
}

// Running: a bright comet (with a softer, wider glow behind it) circles the
// ring. 8° every 40 ms ≈ one lap in 1.8 s, repainting two short arcs.
static void comet_tick(lv_timer_t *t)
{
    (void)t;
    s_comet_angle = (s_comet_angle + 8) % 360;
    lv_arc_set_angles(s_comet_glow, s_comet_angle, s_comet_angle + 64);
    lv_arc_set_angles(s_comet, s_comet_angle + 22, s_comet_angle + 64);
}

static void comet_show(bool on)
{
    set_hidden(s_comet, !on);
    set_hidden(s_comet_glow, !on);
    if (on) lv_timer_resume(s_comet_timer);
    else lv_timer_pause(s_comet_timer);
}

// Not connected: a faint dot sweeps the ring, like a radar looking for a host.
static void radar_tick(lv_timer_t *t)
{
    (void)t;
    if (s_screen != SCR_OFFLINE) return;
    s_radar_angle = (s_radar_angle + 10) % 360;
    lv_arc_set_angles(s_radar, s_radar_angle, s_radar_angle + 26);
}

// Engine tag colours (the engine name is also written: colour is a hint).
static uint32_t engine_color(const char *e)
{
    if (strstr(e, "claude")) return 0xd97757;
    if (strstr(e, "codex") || strstr(e, "openai")) return 0x10a37f;
    if (strstr(e, "gemini")) return 0x4285f4;
    if (strstr(e, "cursor")) return 0xa8b3c1;
    if (strstr(e, "hermes")) return 0xc084fc;
    return 0x64748b;
}

// ── turn timer ──
static uint32_t ui_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ── what the person has NOT looked at yet ───────────────────────────────────
// A finished turn is announced by a toast that lives three seconds, and this
// board has no buzzer: step away and the work of a colleague — or of the
// fourth agent on the carousel — finishes in silence. So the agent keeps a
// flag until someone touches its card: green for a turn that finished, red
// for one that failed. It is the same promise as a question's amber chip.
static struct { char id[ID_MAX]; uint32_t col; } s_unseen[CABLE_MAX_AGENTS];

static void unseen_mark(const char *id, uint32_t col)
{
    if (!id || !id[0]) return;
    int free_slot = -1;
    for (int k = 0; k < CABLE_MAX_AGENTS; k++) {
        if (s_unseen[k].id[0]) {
            if (strcmp(s_unseen[k].id, id) == 0) { s_unseen[k].col = col; return; }
        } else if (free_slot < 0) {
            free_slot = k;
        }
    }
    if (free_slot >= 0) {
        cable_utf8_copy(s_unseen[free_slot].id, sizeof(s_unseen[free_slot].id), id);
        s_unseen[free_slot].col = col;
    }
}

static void unseen_clear(const char *id)
{
    if (!id || !id[0]) return;
    for (int k = 0; k < CABLE_MAX_AGENTS; k++) {
        if (s_unseen[k].id[0] && strcmp(s_unseen[k].id, id) == 0) { s_unseen[k].id[0] = '\0'; return; }
    }
}

/** The colour waiting on [id] — 0 when nothing is. */
static uint32_t unseen_color(const char *id)
{
    if (!id || !id[0]) return 0;
    for (int k = 0; k < CABLE_MAX_AGENTS; k++) {
        if (s_unseen[k].id[0] && strcmp(s_unseen[k].id, id) == 0) return s_unseen[k].col;
    }
    return 0;
}

// An agent that left the tab keeps no flag: the list is the source of truth.
static void unseen_prune(void)
{
    for (int k = 0; k < CABLE_MAX_AGENTS; k++) {
        if (!s_unseen[k].id[0]) continue;
        bool still = false;
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].id, s_unseen[k].id) == 0) { still = true; break; }
        }
        if (!still) s_unseen[k].id[0] = '\0';
    }
}

static void run_track(void)
{
    // Keep the start time of agents that are running, forget the others.
    for (int k = 0; k < CABLE_MAX_AGENTS; k++) {
        if (!s_run[k].id[0]) continue;
        bool still = false;
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].id, s_run[k].id) == 0 && strcmp(s_agents[i].state, "running") == 0) {
                still = true;
                break;
            }
        }
        if (!still) s_run[k].id[0] = '\0';
    }
    for (int i = 0; i < s_agent_count; i++) {
        if (strcmp(s_agents[i].state, "running") != 0) continue;
        int free_slot = -1, found = -1;
        for (int k = 0; k < CABLE_MAX_AGENTS; k++) {
            if (strcmp(s_run[k].id, s_agents[i].id) == 0) { found = k; break; }
            if (!s_run[k].id[0] && free_slot < 0) free_slot = k;
        }
        if (found < 0 && free_slot >= 0) {
            memcpy(s_run[free_slot].id, s_agents[i].id, ID_MAX);
            s_run[free_slot].since = ui_now_ms();
        }
    }
}

static bool run_elapsed(const char *id, uint32_t *secs)
{
    for (int k = 0; k < CABLE_MAX_AGENTS; k++) {
        if (s_run[k].id[0] && strcmp(s_run[k].id, id) == 0) {
            *secs = (ui_now_ms() - s_run[k].since) / 1000;
            return true;
        }
    }
    return false;
}

static void state_line(const cable_agent_t *a)
{
    uint32_t secs;
    if (strcmp(a->state, "running") == 0 && run_elapsed(a->id, &secs)) {
        lv_label_set_text_fmt(s_h_state, LV_SYMBOL_BULLET " %s · %lu:%02lu", label_of_state(a->state),
                              (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    } else {
        lv_label_set_text_fmt(s_h_state, LV_SYMBOL_BULLET " %s", label_of_state(a->state));
    }
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
    if (s_screen == SCR_LAMP) {
        // A lamp does not dim, but it does not burn all night either: dark
        // after HARNESS_LAMP_OFF_AFTER_S untouched, even while a question
        // waits (it stays queued; a new one wakes the screen).
        if (CONFIG_HARNESS_LAMP_OFF_AFTER_S > 0 &&
            idle_ms >= (uint32_t)CONFIG_HARNESS_LAMP_OFF_AFTER_S * 1000u) {
            return 0;
        }
        return s_lamp_level;
    }
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
    if (s_screen == SCR_SETTINGS && lv_slider_is_dragged(s_set_slider)) {   // live preview
        s_bl_now = display_get_backlight();
        return;
    }
    // Fade: waking is quick (≈150 ms), dimming is slow (≈1 s) so the eye
    // follows it instead of seeing the screen "die".
    const int target = power_target();
    if (s_bl_now < 0) s_bl_now = display_get_backlight();
    if (s_bl_now < target) s_bl_now = s_bl_now + 6 > target ? target : s_bl_now + 6;
    else if (s_bl_now > target) s_bl_now = s_bl_now - 1 < target ? target : s_bl_now - 1;
    if (display_get_backlight() != s_bl_now) display_set_backlight(s_bl_now);
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
    // A lamp set lower than the UI brightness is still a lit screen: a touch
    // on it is a gesture, not a wake-up.
    if (s_screen == SCR_LAMP) return display_get_backlight() == 0;
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

static void anim_toast_opa(void *o, int32_t v)
{
    lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0);
    lv_obj_set_style_border_opa(o, (lv_opa_t)v, 0);
    lv_obj_set_style_text_opa(o, (lv_opa_t)v, 0);
}

static void toast_gone(lv_anim_t *a)
{
    lv_obj_t *o = a->var;
    if (o == s_toast) s_toast = NULL;
    lv_obj_delete(o);
}

static void toast_delete(lv_timer_t *t)
{
    (void)t;
    s_toast_timer = NULL;
    if (!s_toast) return;
    lv_anim_t a;                       // fade out, then delete
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, anim_toast_opa);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_completed_cb(&a, toast_gone);
    lv_anim_start(&a);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    s_toast = NULL;                    // a new toast may take the stage meanwhile
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
    fx_start(s_toast, anim_translate_y, 18, 0, 200, 0, lv_anim_path_overshoot);
    fx_start(s_toast, anim_toast_opa, 0, 255, 160, 0, NULL);
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

// ── lamp mode: the dial as a light ─────────────────────────────────────────
// The screen becomes a bulb: a radial glow in one of five tones, the
// backlight at the lamp's own level (never dimmed; dark only after
// HARNESS_LAMP_OFF_AFTER_S untouched, question waiting or not). Drag ↑↓ =
// brightness, ←→ = tone, tap = leave. A question still takes the face, then
// hands it back. A dot at 12 o'clock tells what the agents are doing.

static const struct { const char *fr, *en; uint32_t center, mid, edge; } LAMP_TONES[LAMP_WARMTHS] = {
    { "Bougie", "Candle",   0xffd49a, 0xff9a3c, 0x4a1a03 },
    { "Chaude", "Warm",     0xfff0d8, 0xffc27e, 0x6a320a },
    { "Neutre", "Neutral",  0xfffaf0, 0xffe1b8, 0x5e4a30 },
    { "Jour",   "Daylight", 0xffffff, 0xeef0ff, 0x46506a },
    { "Froide", "Cool",     0xf2f7ff, 0xcfe0ff, 0x2c3c60 },
};

static void lamp_style(void)
{
    const int w = s_lamp_warmth;
    const uint32_t c[3] = { LAMP_TONES[w].center, LAMP_TONES[w].mid, LAMP_TONES[w].edge };
    const uint8_t frac[3] = { 0, 150, 255 };
    for (int i = 0; i < 3; i++) {
        s_lamp_grad.stops[i].color = lv_color_hex(c[i]);
        s_lamp_grad.stops[i].opa = LV_OPA_COVER;
        s_lamp_grad.stops[i].frac = frac[i];
    }
    s_lamp_grad.stops_count = 3;
    lv_grad_radial_init(&s_lamp_grad, LV_GRAD_CENTER, LV_GRAD_CENTER, LV_GRAD_RIGHT, LV_GRAD_CENTER,
                        LV_GRAD_EXTEND_PAD);
    lv_obj_set_style_bg_grad(s_scr_lamp, &s_lamp_grad, 0);
    lv_obj_invalidate(s_scr_lamp);   // same descriptor, new colours
}

// A short line in the glow, fading away.
static void lamp_hint(const char *text, uint32_t hold_ms)
{
    lv_label_set_text(s_lamp_hint, text);
    lv_anim_delete(s_lamp_hint, anim_text_opa);
    lv_obj_set_style_text_opa(s_lamp_hint, LV_OPA_COVER, 0);
    fx_start(s_lamp_hint, anim_text_opa, 255, 0, 600, hold_ms, lv_anim_path_ease_in);
}

static void lamp_hint_level(void)
{
    char t[40];
    snprintf(t, sizeof(t), "%d %%  ·  %s", s_lamp_level,
             TR(LAMP_TONES[s_lamp_warmth].fr, LAMP_TONES[s_lamp_warmth].en));
    lamp_hint(t, 900);
}

// What the agents are doing, as one dot: a question > an error > a turn.
static void lamp_dot_update(void)
{
    uint32_t col = 0;
    if (s_q_pending > 0 || s_notif_questions > 0) {
        col = COL_WAITING;
    } else {
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].state, "error") == 0) { col = COL_ERROR; break; }
            if (strcmp(s_agents[i].state, "running") == 0) col = COL_RUNNING;
        }
    }
    set_hidden(s_lamp_dot, col == 0);
    if (col) lv_obj_set_style_bg_color(s_lamp_dot, lv_color_hex(col), 0);
}

// `touched`: opened by a person (hold, Settings → Lamp, back from a question)
// rather than by lamp_auto(). Only a person restarts the idle count, so the
// lamp's off delay runs from the last touch, not from the automatic switch.
static void open_lamp(bool touched)
{
    static bool explained;
    s_lamp_level = settings_lamp_level();
    s_lamp_warmth = settings_lamp_warmth();
    lamp_style();
    lamp_dot_update();
    load(s_scr_lamp, SCR_LAMP, LV_SCR_LOAD_ANIM_FADE_IN);
    // Opened by a long press, the finger is still down: that contact is spent.
    // Without this, when the press was on a bare screen (offline), LVGL hands
    // it to the lamp and the lift reads as "tap = leave".
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED) {
            lv_indev_wait_release(in);
        }
    }
    if (!explained) {
        lamp_hint(TR("↑↓ intensité  ·  ←→ teinte\ntouchez pour sortir",
                     "↑↓ brightness  ·  ←→ tone\ntap to leave"), 2600);
        explained = true;
    } else {
        lamp_hint_level();
    }
    if (touched) lv_display_trigger_activity(NULL);
    power_update();
}

static void close_lamp(void)
{
    s_lamp_resume = false;
    settings_set_lamp(s_lamp_level, s_lamp_warmth);
    lv_display_trigger_activity(NULL);   // the face is back: the idle count starts over
    go_home(LV_SCR_LOAD_ANIM_FADE_IN);
}

static void lamp_set_warmth(int w)
{
    if (w < 0) w = 0;
    if (w >= LAMP_WARMTHS) w = LAMP_WARMTHS - 1;
    if (w == s_lamp_warmth) return;
    s_lamp_warmth = w;
    lamp_style();
    lamp_hint_level();
}

// One finger: the first 10 px decide the axis, then the drag is live;
// NVS is written once, on release. No travel at all = a tap = leave.
static void lamp_touch(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_lamp_drag.start = p;
        s_lamp_drag.level0 = s_lamp_level;
        s_lamp_drag.warm0 = s_lamp_warmth;
        s_lamp_drag.axis = 0;
        return;
    }
    const int32_t dx = p.x - s_lamp_drag.start.x, dy = p.y - s_lamp_drag.start.y;
    if (code == LV_EVENT_PRESSING) {
        if (!s_lamp_drag.axis && (abs(dx) > 10 || abs(dy) > 10)) s_lamp_drag.axis = abs(dy) >= abs(dx) ? 'y' : 'x';
        if (s_lamp_drag.axis == 'y') {
            int lvl = s_lamp_drag.level0 - dy / 2;          // finger up = brighter
            if (lvl < 5) lvl = 5;
            if (lvl > 100) lvl = 100;
            if (lvl != s_lamp_level) {
                s_lamp_level = lvl;
                s_bl_now = lvl;                             // live, no fade behind the finger
                display_set_backlight(lvl);
                lamp_hint_level();
            }
        } else if (s_lamp_drag.axis == 'x') {
            lamp_set_warmth(s_lamp_drag.warm0 + dx / 40);   // finger → = cooler
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_lamp_drag.axis) {
            if (code == LV_EVENT_RELEASED) close_lamp();
            return;
        }
        settings_set_lamp(s_lamp_level, s_lamp_warmth);
    }
}

static void lamp_long_pressed(lv_event_t *e)
{
    (void)e;
    open_lamp(true);
}

// Left alone on a face at rest (home or offline), the dial becomes the lamp
// (HARNESS_LAMP_AUTO_AFTER_S, 0 = never); a tap gives the face back through
// close_lamp() → go_home(). A waiting question keeps the face, and a screen
// that already went dark (forced off, or the off delay) stays dark.
static void lamp_auto(void)
{
#if CONFIG_HARNESS_LAMP_AUTO_AFTER_S > 0
    if (s_screen != SCR_HOME && s_screen != SCR_OFFLINE) return;
    if (s_q_pending > 0 || s_notif_questions > 0) return;
    if (lv_display_get_inactive_time(NULL) < (uint32_t)CONFIG_HARNESS_LAMP_AUTO_AFTER_S * 1000u) return;
    if (power_target() == 0) return;
    open_lamp(false);   // not a touch: HARNESS_LAMP_OFF_AFTER_S still runs from the last one
#endif
}

static void build_lamp(void)
{
    s_scr_lamp = new_screen();
    lv_obj_set_style_bg_opa(s_scr_lamp, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_scr_lamp, lamp_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr_lamp, lamp_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr_lamp, lamp_touch, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr_lamp, lamp_touch, LV_EVENT_PRESS_LOST, NULL);
    s_lamp_hint = new_label(s_scr_lamp, "", &font_ui_14, 0x3a2410, 190, LV_ALIGN_CENTER, 0, 62);
    lv_obj_set_style_text_align(s_lamp_hint, LV_TEXT_ALIGN_CENTER, 0);
    s_lamp_dot = new_box(s_scr_lamp);
    lv_obj_set_size(s_lamp_dot, 10, 10);
    lv_obj_align(s_lamp_dot, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_radius(s_lamp_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_lamp_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lamp_dot, 2, 0);
    lv_obj_set_style_border_color(s_lamp_dot, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_opa(s_lamp_dot, LV_OPA_60, 0);
    set_hidden(s_lamp_dot, true);
    lamp_style();
}

// ── home screen ─────────────────────────────────────────────────────────────

static void home_select(int index, bool send_focus)
{
    if (s_agent_count == 0) return;
    index %= s_agent_count;
    if (index < 0) index += s_agent_count;
    const bool moved = index != s_index;
    // Direction of travel, the short way round the carousel.
    int dir = index > s_index ? 1 : -1;
    if (s_agent_count > 2 && abs(index - s_index) > s_agent_count / 2) dir = -dir;
    s_index = index;
    s_stop_armed = false;
    render_home();
    if (moved && s_screen == SCR_HOME) fx_slide_card(dir);
    // The tile the carousel settled on — where the user is LOOKING.
    if (moved && send_focus) cable_client_send_focus(s_agents[s_index].id);
}

static void home_open_current(void)
{
    if (s_agent_count == 0) return;
    // The touch IS the acknowledgement: the flag is spent the moment the
    // person opens the agent whose work it announced.
    unseen_clear(s_agents[s_index].id);
    if (s_screen == SCR_HOME) render_home();
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
    lv_obj_set_style_arc_opa(s_ring, s_pulse_on ? LV_OPA_COVER : LV_OPA_30, LV_PART_INDICATOR);
}

static void busy_tick(lv_timer_t *t)
{
    (void)t;
    if (s_screen == SCR_LAMP) { lamp_dot_update(); return; }
    if (s_screen != SCR_HOME || s_agent_count == 0 ||
        strcmp(s_agents[s_index].state, "running") != 0) {
        return;
    }
    state_line(&s_agents[s_index]);   // "● En cours · 1:23"
}

static void build_home(void)
{
    s_scr_home = new_screen();
    lv_obj_add_event_cb(s_scr_home, home_gesture, LV_EVENT_GESTURE, NULL);

    // Status ring: a dim track, the state-coloured arc over it, and — while a
    // turn runs — a comet with a soft glow circling on top.
    s_ring_track = new_arc(s_scr_home, RING_D, RING_W, COL_LINE, LV_OPA_COVER);
    lv_obj_set_style_arc_opa(s_ring_track, LV_OPA_TRANSP, LV_PART_INDICATOR);
    s_ring = new_arc(s_scr_home, RING_D, RING_W, COL_IDLE, LV_OPA_TRANSP);
    s_comet_glow = new_arc(s_scr_home, RING_D + 4, RING_W + 6, 0x93c5fd, LV_OPA_TRANSP);
    lv_obj_set_style_arc_opa(s_comet_glow, LV_OPA_30, LV_PART_INDICATOR);
    s_comet = new_arc(s_scr_home, RING_D, RING_W, 0xdbeafe, LV_OPA_TRANSP);
    s_comet_timer = lv_timer_create(comet_tick, 40, NULL);
    comet_show(false);

    s_h_header = new_label(s_scr_home, "", &font_ui_14, COL_DIM, 104, LV_ALIGN_TOP_MID, 0, 24);
    lv_label_set_long_mode(s_h_header, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_h_header, 18);
    s_h_state = new_label(s_scr_home, "", &font_ui_14, COL_IDLE, 150, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_long_mode(s_h_state, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_h_state, 18);

    // Fleet badge (agents.end.total), at 7:30 on the dial: centre 90 px from
    // the middle + 14 px radius stays inside the ring (r 110), clear of the
    // state line, the page dots and the action chip.
    s_badge = new_box(s_scr_home);
    lv_obj_set_size(s_badge, 28, 28);
    lv_obj_set_pos(s_badge, 42, 170);
    lv_obj_set_style_radius(s_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_badge, lv_color_hex(0x1f6feb), 0);
    lv_obj_set_style_bg_opa(s_badge, LV_OPA_COVER, 0);
    s_badge_label = lv_label_create(s_badge);
    lv_obj_center(s_badge_label);

    // The "you have not looked at this" flag, opposite the fleet badge (4:30):
    // a filled disc that stays until the card is touched. Not clickable on
    // purpose — a tap over it belongs to the card underneath, like everywhere
    // else on this screen.
    s_unseen_dot = new_box(s_scr_home);
    lv_obj_set_size(s_unseen_dot, 22, 22);
    lv_obj_set_pos(s_unseen_dot, 176, 166);
    lv_obj_set_style_radius(s_unseen_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_unseen_dot, lv_color_hex(COL_DONE), 0);
    lv_obj_set_style_bg_opa(s_unseen_dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_unseen_dot, LV_OBJ_FLAG_CLICKABLE);
    set_hidden(s_unseen_dot, true);

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
    lv_obj_add_event_cb(s_card, lamp_long_pressed, LV_EVENT_LONG_PRESSED, NULL);   // hold = lamp
    lv_obj_add_event_cb(s_scr_home, lamp_long_pressed, LV_EVENT_LONG_PRESSED, NULL);

    s_h_name = new_label(s_card, "", &font_ui_20, COL_TEXT, TEXT_W, LV_ALIGN_TOP_MID, 0, 0);
    lv_label_set_long_mode(s_h_name, LV_LABEL_LONG_DOT);   // height set per text: set_name()
    // Engine pill + machine name.
    s_h_subrow = new_box(s_card);
    lv_obj_set_size(s_h_subrow, TEXT_W, 18);
    lv_obj_set_flex_flow(s_h_subrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_h_subrow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_h_subrow, 5, 0);
    s_h_engine = lv_label_create(s_h_subrow);
    lv_obj_set_style_radius(s_h_engine, 8, 0);
    lv_obj_set_style_bg_opa(s_h_engine, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_h_engine, 6, 0);
    lv_obj_set_style_text_color(s_h_engine, lv_color_hex(COL_BG), 0);
    s_h_machine = lv_label_create(s_h_subrow);
    lv_obj_set_style_text_color(s_h_machine, lv_color_hex(COL_DIM), 0);
    lv_label_set_long_mode(s_h_machine, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_h_machine, 18);
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
        set_hidden(s_h_engine, true);
        lv_label_set_text(s_h_machine, "");
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
        state_line(a);
        set_name(a->name);
        set_hidden(s_h_engine, !a->engine[0]);
        lv_label_set_text(s_h_engine, a->engine);
        lv_obj_set_style_bg_color(s_h_engine, lv_color_hex(engine_color(a->engine)), 0);
        // The machine takes what the pill leaves (measured: LONG_DOT cannot
        // size itself, see set_name()).
        lv_point_t pill;
        lv_text_get_size(&pill, a->engine, &font_ui_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const int32_t room = TEXT_W - (a->engine[0] ? pill.x + 12 + 5 : 0);
        lv_point_t mach;
        lv_text_get_size(&mach, a->machine, &font_ui_14, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_width(s_h_machine, mach.x < room ? mach.x : room);
        lv_label_set_text(s_h_machine, a->machine);
        lv_label_set_text(s_h_summary, a->summary);
    }
    if (question_waiting) ring = COL_WAITING;
    if (!lv_anim_get(s_ring, anim_arc_end)) {          // not while the "done" sweep plays
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(ring), LV_PART_INDICATOR);
        lv_arc_set_angles(s_ring, 0, 360);
    }
    comet_show(!question_waiting && s_agent_count > 0 &&
               strcmp(s_agents[s_index].state, "running") == 0);

    if (question_waiting) {
        lv_timer_resume(s_pulse_timer);
    } else {
        lv_timer_pause(s_pulse_timer);
        s_pulse_on = true;
        lv_obj_set_style_arc_opa(s_ring, LV_OPA_COVER, LV_PART_INDICATOR);
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
        // A page holding unseen work takes the flag's colour: that is how the
        // person finds WHICH agent moved without walking the whole carousel.
        const uint32_t wait = i < s_agent_count ? unseen_color(s_agents[i].id) : 0;
        lv_obj_set_width(d, i == s_index ? 14 : 6);   // the current page is a pill
        lv_obj_set_style_bg_color(d, lv_color_hex(wait ? wait : (i == s_index ? COL_TEXT : COL_LINE)), 0);
    }

    // The flag itself, on the card the person is looking at — until they touch it.
    const uint32_t unseen = s_agent_count > 0 ? unseen_color(s_agents[s_index].id) : 0;
    set_hidden(s_unseen_dot, unseen == 0);
    if (unseen) lv_obj_set_style_bg_color(s_unseen_dot, lv_color_hex(unseen), 0);

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
    run_track();
    unseen_prune();
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
    fx_rise(s_q_header, 0);
    fx_rise(s_q_text, 40);

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
        fx_rise(b, 90 + 45 * (uint32_t)i);
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
        fx_rise(l, 120);
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

static void settings_lamp_clicked(lv_event_t *e)
{
    (void)e;
    open_lamp(true);
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

    s_set_info = new_label(s_scr_settings, "", &font_ui_14, COL_DIM, 176, LV_ALIGN_TOP_MID, 0, 131);
    lv_obj_t *b = new_button(s_scr_settings, LV_SYMBOL_UP, 56, 30, COL_SURFACE2, COL_TEXT,
                             back_home_clicked, NULL);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 34, -16);
    lv_obj_t *lb = new_button(s_scr_settings, TR("Lampe", "Lamp"), 64, 30, 0xffc27e, 0x3a2410,
                              settings_lamp_clicked, NULL);
    lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, -34, -16);

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
        lv_anim_delete(s_pad_puck, anim_translate_y);
        set_hidden(s_pad_puck, false);
        lv_obj_set_style_translate_y(s_pad_puck, p.y - 120, 0);
    } else if (code == LV_EVENT_PRESSING && s_pad.active) {
        pad_sample(now, p.y);
        lv_obj_set_style_translate_y(s_pad_puck, p.y - 120, 0);
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
        // The puck glides back to the middle, like the page settling.
        fx_start(s_pad_puck, anim_translate_y, p.y - 120, 0, 260, 0, NULL);
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
    s_pad_puck = new_box(s_scr_pad);
    lv_obj_set_size(s_pad_puck, 30, 30);
    lv_obj_center(s_pad_puck);
    lv_obj_set_style_radius(s_pad_puck, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pad_puck, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_pad_puck, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_pad_puck, 2, 0);
    lv_obj_set_style_border_color(s_pad_puck, lv_color_hex(COL_ACCENT), 0);
    set_hidden(s_pad_puck, true);
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
    s_boot_arc = new_arc(s_scr_boot, RING_D, 4, COL_ACCENT, LV_OPA_TRANSP);
    lv_arc_set_angles(s_boot_arc, 0, 0);
    s_boot_logo = new_label(s_scr_boot, "λ", &font_logo_72, COL_ACCENT, 0, LV_ALIGN_CENTER, 0, -40);
    new_label(s_scr_boot, "Harness C3", &font_ui_20, COL_TEXT, 170, LV_ALIGN_CENTER, 0, 24);
    lv_obj_t *v = new_label(s_scr_boot, "", &font_ui_14, COL_DIM, 170, LV_ALIGN_CENTER, 0, 52);
    lv_label_set_text_fmt(v, "v%s", s_fw);
    fx_start(s_boot_arc, anim_arc_end, 0, 360, 1000, 0, lv_anim_path_ease_in_out);
    lv_obj_set_style_text_opa(s_boot_logo, LV_OPA_TRANSP, 0);
    fx_start(s_boot_logo, anim_text_opa, 0, 255, 600, 150, NULL);
    fx_start(s_boot_logo, anim_translate_y, 10, 0, 600, 150, NULL);
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
    s_radar = new_arc(s_scr_offline, RING_D, 4, COL_DIM, LV_OPA_TRANSP);
    lv_arc_set_angles(s_radar, 0, 26);
    s_radar_timer = lv_timer_create(radar_tick, 100, NULL);
    lv_obj_add_event_cb(s_scr_offline, lamp_long_pressed, LV_EVENT_LONG_PRESSED, NULL);   // hold = lamp
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
    if (s_lamp_resume) {                 // back to the lamp the question interrupted
        s_lamp_resume = false;
        open_lamp(true);
        return;
    }
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
    mark(DIRTY_AGENTS);
    if (!notify && !beep) return;   // plain state moves ride on the dirty bit
    ui_ev_t ev = { .type = UI_EV_AGENT_EVENT };
    cable_utf8_copy(ev.d.agent.id, sizeof(ev.d.agent.id), agent_id);
    cable_utf8_copy(ev.d.agent.state, sizeof(ev.d.agent.state), state);
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
                if (s_screen != SCR_BOOT && s_screen != SCR_SETTINGS && s_screen != SCR_LAMP) {
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
        // The flag lives longer than the toast and on the agent's own card:
        // green when a turn finished, red when it failed. Restores, quiet
        // summaries and silent sub-agent turns never reach this line, so
        // history refills do not light anything up.
        unseen_mark(ev->d.agent.id, strcmp(ev->d.agent.state, "error") == 0 ? COL_ERROR : COL_DONE);
        // The event arrives AFTER this tick's dirty pass has already painted,
        // and the loop's second pass returns early when nothing else is dirty:
        // without this the flag waits for the next unrelated repaint and the
        // person sees a card with no flag on it.
        if (s_screen == SCR_HOME) render_home();
        ui_wake();
        const char *name = ev->d.agent.id;
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].id, ev->d.agent.id) == 0) { name = s_agents[i].name; break; }
        }
        char msg[ID_MAX + 100];
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK " %s\n%s", name, ev->d.agent.text);
        toast_show(msg, COL_DONE);
        if (s_screen == SCR_HOME && s_agent_count > 0 &&
            strcmp(s_agents[s_index].id, ev->d.agent.id) == 0) {
            comet_show(false);
            fx_ring_sweep(COL_DONE);
        }
        break;
    }
    case UI_EV_QUESTION:
        buzzer_beep(BUZZER_BEEP_QUESTION);
        ui_wake();
        s_q_pending = cable_client_question_count();
        // A question is a job: it takes the face — unless one is already
        // being answered, in which case the header's "+n" says there is more.
        if (s_screen == SCR_LAMP) s_lamp_resume = true;
        if (s_screen != SCR_QUESTION) question_open();
        else question_render();
        break;
    case UI_EV_TOAST:
        buzzer_beep(BUZZER_BEEP_ERROR);
        toast_show(ev->d.toast.text, COL_ERROR);
        // `turn.error` flags its agent through here — the toast carries the
        // text but no id, and the failed agent is the one in the `error` state
        // the daemon just set. That failure is exactly what must survive the
        // toast: a stopped quota is worth a red dot, not three seconds.
        for (int i = 0; i < s_agent_count; i++) {
            if (strcmp(s_agents[i].state, "error") == 0) { unseen_mark(s_agents[i].id, COL_ERROR); break; }
        }
        if (s_screen == SCR_HOME) { render_home(); fx_shake(s_card); }
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
    case SCR_LAMP:
        if (ev == BTN_EVENT_B_SHORT) close_lamp();
        else if (ev == BTN_EVENT_A_SHORT) {
            lamp_set_warmth((s_lamp_warmth + 1) % LAMP_WARMTHS);
            settings_set_lamp(s_lamp_level, s_lamp_warmth);
        }
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
    lamp_auto();
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
    build_lamp();
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

#ifdef HARNESS_UI_SIM
// Simulator only: which screen is up (the tests cannot see the static state).
const char *ui_sim_screen(void)
{
    static const char *const names[] = { "boot", "offline", "home", "question", "settings", "pad", "lamp" };
    return names[s_screen];
}
#endif
