// The USB transport under cable_frame. Adapted from
// devices/harness-device/firmware/main/cable_link.c of
// autonomous-ai/openharness (MIT); see cable_link.h for the port notes.

#include "cable_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cable";

// Driver FIFOs.
//
// ⚠️ RX IS NOT A CONVENIENCE — IT IS THE ONLY THING BETWEEN A SLOW READER AND
// SILENT DATA LOSS. There is NO back-pressure on this peripheral: the IDF
// driver's ISR drains the hardware FIFO unconditionally and ignores the
// ringbuffer's return value, so a full ring means those bytes are simply
// gone, and the host is never NAK'd and never learns (PROTOCOL.md §7.3).
//
// Measured by the sibling firmware, not theorised: with a smaller ring a
// firmware transfer died at 0 of 1,342,160 bytes written. So the rule this
// file keeps: RX_BUF ≥ the largest number of bytes the peer may have in
// flight while the reader task is blocked. The (upstream) firmware transfer
// is the only sender that can saturate the link, and its credit window is
// 16 KB; 32 KB is that window twice over, for scheduling jitter. Even though
// v1 never accepts firmware slices, the window is kept — a future C3-aware
// updater depends on it, and the RAM is budgeted (SPEC.md §3).
#define USJ_RX_BUF (32 * 1024)

// TX ring: holds the LARGEST POSSIBLE FRAME. The driver's ring is
// all-or-nothing per write and refuses outright any single write larger than
// itself — with the old 2048 B ring a large `answer` (4 questions ×
// multi-select labels ≈ 2.4 KB) failed EVERY time, silently. Sized to
// CABLE_MAX_FRAME, every frame goes in as ONE ring item: atomic on the wire,
// so console text (routed through the driver below) can only ever land
// between two frames, never inside one.
//
// The protocol allows 8 KB payloads, but the largest thing THIS firmware
// sends is an `answer` (≤ 4 questions × one key + joined labels ≈ 2.6 KB);
// hello/focus/scroll are under 200 B and a LOG line is capped at 512 B. So
// outbound frames are capped at 4 KB, which halves the RAM the TX side costs
// (encode buffer + ring) on a chip with ~320 KB in total.
#define TX_PAYLOAD_MAX 4096
#define TX_FRAME_MAX   (CABLE_HEADER_BYTES + TX_PAYLOAD_MAX + CABLE_CRC_BYTES)
#define USJ_TX_BUF     (TX_FRAME_MAX + 256)

// One read's worth of bytes off the port. Small on purpose: the decoder is
// where reassembly happens; this only keeps the syscall rate sane.
#define READ_CHUNK 256

// How long a read parks waiting for bytes. Not a poll interval — the driver
// wakes the task as soon as anything arrives.
#define READ_WAIT_MS 100

// How long a write waits for the host to make room. Finite, and that is the
// point: an unplugged cable fills the TX FIFO and never drains it, and
// portMAX_DELAY there parks whatever task called send() forever. "Nobody is
// reading" has to be an ordinary, survivable answer.
#define WRITE_WAIT_MS 100

// A log line waits far less than a real message. Logging must never become
// back-pressure: ESP_LOG is called from every task in the system, and at
// WRITE_WAIT_MS an unread port would stall each of them per line.
#define LOG_WRITE_WAIT_MS 5

// Reader task stack. The frame callback runs on this task, so it carries
// whatever the message layer does — cJSON parsing and UI event posts.
#define READER_STACK 6144

// One log line's worth of formatted text. Anything longer is truncated rather
// than split (PROTOCOL.md §9): half a line that arrives is worth more than a
// mechanism that could deadlock producing the other half.
#define LOG_LINE_MAX 512

static cable_decoder_t s_decoder;
static cable_frame_cb  s_cb;
static void          (*s_tick)(void);
static void           *s_ctx;
static bool            s_running;

// Serialises the shared encode buffer AND the write, so two tasks sending at
// once cannot interleave halves of two frames onto the wire. A frame split by
// a second sender is unrecoverable — both halves have valid magic and neither
// has a valid CRC (PROTOCOL.md §10.3).
static SemaphoreHandle_t s_tx_lock;

// BSS rather than a stack array (it would not fit) or a malloc (a failed
// allocation mid-session is a worse outcome than a known, always-paid 4 KB).
// A -1 from cable_frame_encode means exactly one thing: the payload is over
// TX_PAYLOAD_MAX, a bug on this side (see cable_link_send).
static uint8_t s_tx_frame[TX_FRAME_MAX];

// ── log framing ─────────────────────────────────────────────────────────────

static vprintf_like_t s_prev_vprintf;

static bool send_locked(uint8_t type, const uint8_t *payload, size_t payload_len, TickType_t wait)
{
    int len = cable_frame_encode(type, payload, payload_len, s_tx_frame, sizeof(s_tx_frame));
    if (len < 0) return false;
    // One write per frame: all of it or none of it (see USJ_TX_BUF).
    return usb_serial_jtag_write_bytes(s_tx_frame, (size_t)len, wait) == len;
}

static int log_vprintf(const char *fmt, va_list args)
{
    // From an ISR there is nothing safe to do here — the mutex would abort —
    // and panic/early-boot output does not come through this hook anyway.
    // Reentrancy (a log emitted while THIS task holds the TX lock, e.g. by
    // the driver itself) falls through to the plain console instead of
    // deadlocking; another task's log simply waits its (short) turn.
    if (xPortInIsrContext() || !s_tx_lock ||
        xSemaphoreGetMutexHolder(s_tx_lock) == xTaskGetCurrentTaskHandle()) {
        return s_prev_vprintf ? s_prev_vprintf(fmt, args) : 0;
    }

    static char line[LOG_LINE_MAX];   // guarded by s_tx_lock, like s_tx_frame
    if (!s_running || xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(LOG_WRITE_WAIT_MS)) != pdTRUE) {
        return s_prev_vprintf ? s_prev_vprintf(fmt, args) : 0;
    }
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1;
        // The trailing newline is the console's business, not the protocol's:
        // the frame IS the line.
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        if (len > 0) send_locked(CABLE_TYPE_LOG, (const uint8_t *)line, len,
                                 pdMS_TO_TICKS(LOG_WRITE_WAIT_MS));
    }
    xSemaphoreGive(s_tx_lock);
    return n;
}

void cable_link_set_log_framing(bool on)
{
    if (!s_running) return;
    if (on) {
        if (!s_prev_vprintf) s_prev_vprintf = esp_log_set_vprintf(log_vprintf);
        return;
    }
    // Restore the plain console FIRST, then say so — the line explaining what
    // happened must be readable by whoever is now watching with a monitor.
    if (s_prev_vprintf) {
        esp_log_set_vprintf(s_prev_vprintf);
        s_prev_vprintf = NULL;
    }
}

// ── link ────────────────────────────────────────────────────────────────────

static void reader_task(void *arg)
{
    (void)arg;
    uint8_t chunk[READ_CHUNK];
    TickType_t last_tick = 0;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(READ_WAIT_MS));
        if (n > 0) {
            // Never fails and never rejects: everything arriving here is
            // untrusted, starts mid-stream after every boot, and the only
            // useful response to a byte that makes no sense is to step over it.
            cable_decoder_feed(&s_decoder, chunk, (size_t)n, s_cb, s_ctx);
        }
        const TickType_t now = xTaskGetTickCount();
        if (s_tick && (now - last_tick) >= pdMS_TO_TICKS(CABLE_LINK_TICK_MS)) {
            last_tick = now;
            s_tick();
        }
    }
}

bool cable_link_start(cable_frame_cb cb, void (*tick)(void), void *ctx)
{
    if (s_running) return true;

    s_cb = cb;
    s_tick = tick;
    s_ctx = ctx;
    cable_decoder_init(&s_decoder);

    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        ESP_LOGE(TAG, "no memory for the tx lock — link disabled");
        return false;
    }

    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = USJ_TX_BUF,
        .rx_buffer_size = USJ_RX_BUF,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        // Name the cause: the symptom lands far from here (the dial draws its
        // screen and simply never hears from the machine).
        ESP_LOGE(TAG, "usb_serial_jtag driver install failed (%s) — no link to the daemon",
                 esp_err_to_name(err));
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        return false;
    }

    // Route the console (printf, and ESP_LOG while not framed) through the
    // driver. Left on the default "no driver" path, console text is pushed
    // straight into the hardware FIFO and can land in the MIDDLE of a frame
    // the driver's ISR is sending — the peer then loses that frame. Through
    // the driver, every write is an atomic ring-buffer item, so console text
    // can only fall between frames. (It never blocks: the VFS drops output
    // after one 50 ms timeout while nobody is reading.)
    usb_serial_jtag_vfs_use_driver();

    if (xTaskCreate(reader_task, "cable_link", READER_STACK, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "reader task create failed — no link to the daemon");
        usb_serial_jtag_vfs_use_nonblocking();
        usb_serial_jtag_driver_uninstall();
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        return false;
    }

    s_running = true;
    ESP_LOGI(TAG, "usb link up on the native port (frame v%d, max frame %d B)",
             CABLE_FRAME_VERSION, CABLE_MAX_FRAME);
    return true;
}

bool cable_link_send(uint8_t type, const uint8_t *payload, size_t payload_len)
{
    if (!s_running) return false;

    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    const bool ok = send_locked(type, payload, payload_len, pdMS_TO_TICKS(WRITE_WAIT_MS));
    const bool too_big = payload_len > TX_PAYLOAD_MAX;
    xSemaphoreGive(s_tx_lock);

    if (ok) return true;
    if (too_big) {
        // A bug on this side, and one the far end could only ever report back
        // as noise, so it has to be caught and named here.
        ESP_LOGE(TAG, "refusing to send %u-byte payload (max %d)",
                 (unsigned)payload_len, TX_PAYLOAD_MAX);
        return false;
    }
    // DEBUG, not WARN. "Nobody is draining the port" is this device's resting
    // state, not a fault: it is unplugged, or in front of a machine with no
    // daemon. At WARN the log would be a wall of identical lines whenever
    // nothing is wrong.
    ESP_LOGD(TAG, "short write (host not reading)");
    return false;
}

bool cable_link_host_present(void)
{
    // Guarded on s_running: the driver call is undefined before install, and
    // "no driver" is indistinguishable from "no host" to everything above.
    return s_running && usb_serial_jtag_is_connected();
}

void cable_link_counters(uint32_t *corrupt_frames, uint32_t *discarded_bytes)
{
    if (corrupt_frames) *corrupt_frames = s_decoder.corrupt_frames;
    if (discarded_bytes) *discarded_bytes = s_decoder.discarded_bytes;
}

void cable_link_reset_decoder(void)
{
    cable_decoder_reset(&s_decoder);
}
