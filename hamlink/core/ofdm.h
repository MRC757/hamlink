/* SPDX-License-Identifier: GPL-3.0-or-later */
/* COFDMTV (Rattlegram) modem wrapper: 48 kHz mono int16 <-> packets of up to 170 bytes.
 * Each packet = [noise lead-in symbols] + sync + preamble + 4 payload symbols + 1 tail symbol,
 * 180 ms per symbol, ~1.26 s without lead-in. Polar code with CRC32C protects the payload.
 * Packet sizes: <=85 B uses the most robust code, <=128 B medium, <=170 B the highest rate. */
#ifndef HAMLINK_OFDM_H
#define HAMLINK_OFDM_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OFDM_MAX_PAYLOAD 170
#define OFDM_SYMBOL_SAMPLES 8640 /* 180 ms at 48 kHz (160 ms symbol + 20 ms guard) */
#define OFDM_CARRIER_HZ 1500     /* centre of the 1600 Hz wide signal (700..2300 Hz) */
#define OFDM_RX_QUEUE 8

typedef struct ofdm_tx ofdm_tx_t;
typedef struct ofdm_rx ofdm_rx_t;

ofdm_tx_t *ofdm_tx_create(void);
void ofdm_tx_destroy(ofdm_tx_t *t);
/* Samples a packet with `noise_symbols` lead-in symbols would need. */
size_t ofdm_packet_samples(int noise_symbols);
/* Modulate one packet. len in 1..170. callsign (<=9 chars A-Z0-9) is carried in the packet header.
 * Returns samples written, 0 on error / cap too small. */
size_t ofdm_tx_modulate(ofdm_tx_t *t, const uint8_t *payload, size_t len, const char *callsign,
                        int noise_symbols, int16_t *out, size_t cap);

ofdm_rx_t *ofdm_rx_create(void);
void ofdm_rx_destroy(ofdm_rx_t *r);
void ofdm_rx_process(ofdm_rx_t *r, const int16_t *pcm, size_t n);
/* Pops one decoded packet. Returns payload length (85, 128 or 170) or 0 if none queued.
 * callsign receives up to 9 chars + NUL, flips the number of bit errors the decoder corrected. */
int ofdm_rx_next(ofdm_rx_t *r, uint8_t payload[OFDM_MAX_PAYLOAD], char callsign[10], int *flips);
/* ---- bundling: several mesh frames per packet ----
 * Packet payload = 0xB1, then records [len][frame bytes] (len 1..169), zero padded.
 * 0xB1 can never start valid UTF-8 text, so stock Rattlegram messages are not mistaken for bundles. */
#define OFDM_BUNDLE_MAGIC 0xB1
#define OFDM_TAIL_KEEP_SAMPLES 960 /* fade-out kept at the end of non-final packets in a burst */

/* Number of packets `n` frames would need (greedy, in order). Frames longer than 168 B are skipped. */
int ofdm_count_packets(const size_t *lens, int n);
/* Upper bound on samples ofdm_encode_burst needs for these frames. */
size_t ofdm_burst_max_samples(const size_t *lens, int n, int noise_symbols);
/* Pack the frames and modulate the packets back to back: lead-in noise only before the first packet;
 * every non-final packet drops most of its trailing silence symbol. Returns samples written. */
size_t ofdm_encode_burst(ofdm_tx_t *t, const uint8_t *const *frames, const size_t *lens, int n,
                         const char *callsign, int noise_symbols, int16_t *out, size_t cap);
/* Split a received packet into frames (pointers into payload). Returns the count, 0 if not a bundle. */
int ofdm_unpack(const uint8_t *payload, int len, const uint8_t **frames, size_t *lens, int max);

uint32_t ofdm_rx_packets_ok(const ofdm_rx_t *r);
uint32_t ofdm_rx_packets_failed(const ofdm_rx_t *r);

#ifdef __cplusplus
}
#endif
#endif
