/* SPDX-License-Identifier: GPL-3.0-or-later */
/* KISS + AX.25 UI framing for radios with a built-in TNC (Bluetooth SPP / serial KISS).
 * Each mesh frame travels as one AX.25 UI frame (dest HAMLNK, PID 0xF0) inside one KISS data frame, so
 * the radio's own modem does HDLC/CRC/AFSK and the on-air frames are valid AX.25 that Direwolf etc. can see. */
#ifndef HAMLINK_TNC_H
#define HAMLINK_TNC_H
#include <stddef.h>
#include <stdint.h>

#define KISS_FEND 0xC0
#define KISS_FESC 0xDB
#define KISS_TFEND 0xDC
#define KISS_TFESC 0xDD
#define KISS_MAX_FRAME 600
#define AX25_HDR_LEN 16 /* dest 7 + src 7 + control 1 + PID 1 (no digipeaters) */
#define AX25_DEST "HAMLNK"

/* Escaped KISS frame: FEND, cmd byte (port<<4 | command), data, FEND. Returns bytes written, 0 if cap too small. */
size_t kiss_encode(uint8_t port, uint8_t command, const uint8_t *data, size_t len, uint8_t *out, size_t cap);

typedef struct {
    uint8_t buf[KISS_MAX_FRAME];
    size_t n;
    int in_frame, esc;
    uint32_t overflows;
} kiss_dec_t;

typedef void (*kiss_frame_cb)(void *user, uint8_t port, uint8_t command, const uint8_t *data, size_t len);
void kiss_dec_init(kiss_dec_t *d);
/* Feed raw bytes from the link (any chunking). Calls cb for each complete frame. */
void kiss_dec_feed(kiss_dec_t *d, const uint8_t *bytes, size_t n, kiss_frame_cb cb, void *user);

/* AX.25 UI frame (no FCS). src_call like "W1AAA", "W1AAA-7" or "W1AAA/P" (only the first 6 A-Z0-9 are used
 * on the wire; the full callsign is inside the mesh header anyway). Returns bytes written, 0 on error. */
size_t ax25_ui_wrap(const char *src_call, const uint8_t *info, size_t len, uint8_t *out, size_t cap);
/* Accepts only UI frames (control 0x03, PID 0xF0) addressed to HAMLNK; digipeater paths are skipped.
 * Returns 1 and sets info/info_len, else 0. src (>= 10 bytes) gets "CALL-SSID" for logging. */
int ax25_ui_unwrap(const uint8_t *frame, size_t len, const uint8_t **info, size_t *info_len, char *src);

#endif
