// The USB transport under cable_frame: bytes in from the native USB
// (USB-Serial-JTAG) port, frames out to it.
//
// Adapted from devices/harness-device/firmware/main/cable_link.h of
// autonomous-ai/openharness (MIT). The ESP32-C3 has the same ROM
// USB-Serial-JTAG peripheral as the reference ESP32-S3 and enumerates with
// the same 303a:1001 identity (the descriptor cannot be changed), so the
// transport applies unchanged — PROTOCOL.md §1, §10.
//
// ── ONE PORT, AND WHAT THAT COSTS ───────────────────────────────────────────
// This board has exactly ONE USB port, so the console and this protocol share
// a wire. That is why cable_frame carries a LOG type: while a session is up,
// every ESP_LOG line goes out as a CABLE_TYPE_LOG frame and the daemon files
// it. What CANNOT be framed is the residue, by design: the ROM, the
// second-stage bootloader and the panic handler all write to this port
// directly. The far end's decoder is built to walk through it — that is what
// the magic scan and the CRC are for.
//
// ── WHEN NOBODY IS LISTENING, THE CONSOLE IS A CONSOLE ─────────────────────
// Log framing is installed when a session starts and removed when it ends
// (cable_link_set_log_framing). Unplugged, or plugged into a machine with no
// daemon running, the port carries ordinary console text and `idf.py monitor`
// behaves exactly as it always has.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cable_frame.h"

// Install the USB-Serial-JTAG driver, route the console through it, and start
// the reader task.
//
// `cb` is invoked once per decoded frame, ON THE READER TASK, with the
// payload pointing into the decoder's own buffer — valid only for the
// duration of the call; copy anything that must outlive it. It must never
// touch LVGL: post to the UI task instead.
//
// `tick` (may be NULL) is called on the same task at least every
// CABLE_LINK_TICK_MS, whether or not bytes arrive. The session machine runs
// there, so it can never race the frame handlers.
//
// Returns false if the driver would not install, which leaves the dial
// running with no link rather than failing to boot: a device that shows
// "Not connected" is diagnosable from across the room, a boot loop is not.
#define CABLE_LINK_TICK_MS 100
bool cable_link_start(cable_frame_cb cb, void (*tick)(void), void *ctx);

// Frame `payload` and write it to the port. Returns true when the whole
// frame went out.
//
// A false return means the host is not draining the port (an unopened or
// unplugged CDC endpoint fills the FIFO and the write times out). That is a
// normal state for this device, not an error — callers treat it as
// "not connected" and never retry in a tight loop.
//
// If a write is cut short mid-frame the peer resyncs past the truncated
// frame on the next magic — that recovery is exactly what the
// CRC-plus-magic-scan exists for, so a bad moment costs one message rather
// than the link.
bool cable_link_send(uint8_t type, const uint8_t *payload, size_t payload_len);

// Route ESP_LOG through the link as CABLE_TYPE_LOG frames (true), or back to
// the plain console (false). Called by the session layer, not by this one:
// whether a peer is listening is a message-layer fact (a `welcome` arrived).
void cable_link_set_log_framing(bool on);

// Whether a USB HOST is currently driving this port.
//
// NOT the same question as "is the daemon talking to me": the driver answers
// from USB frame activity, so it is true whenever the cable is in a running
// computer — including one with no daemon. It is useful for exactly one
// thing: noticing that the cable left, or that the machine went to sleep. A
// session that ends because the DAEMON quit with the cable still in is
// invisible here — the silence window in cable_client.c stands in.
bool cable_link_host_present(void);

// Framing health. The RATE is the diagnosis, not the totals: a handful of
// discarded bytes right after boot is the bootloader's parting words and is
// expected; a steady trickle during a session means the two sides disagree
// about the format or the cable is bad.
void cable_link_counters(uint32_t *corrupt_frames, uint32_t *discarded_bytes);

// Drop any half-received frame, e.g. when a session has ended. Call it FROM
// THE FRAME CALLBACK: the decoder has one reader — the link task — and no
// lock; resetting it from another task mid-feed corrupts the buffer.
void cable_link_reset_decoder(void);
