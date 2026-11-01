/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Voice clips: 8 kHz mono PCM <-> Codec2 700C frames (4 bytes / 40 ms = 100 B/s).
 * Clip byte stream: [format id][frame0][frame1]...  Format id lets us add modes later. */
#ifndef HAMLINK_VOICE_H
#define HAMLINK_VOICE_H
#include <stddef.h>
#include <stdint.h>

#define VOICE_FMT_C2_700C 1
#define VOICE_SAMPLE_RATE 8000
#define VOICE_FRAME_SAMPLES 320 /* 40 ms */
#define VOICE_FRAME_BYTES 4
/* Longest clip that fits in one mesh message (32 fragments * 96 B). */
#define VOICE_MAX_CLIP_BYTES (32 * 96)
#define VOICE_MAX_FRAMES ((VOICE_MAX_CLIP_BYTES - 1) / VOICE_FRAME_BYTES)

typedef struct voice_codec voice_codec_t;
voice_codec_t *voice_create(void);
void voice_destroy(voice_codec_t *v);
/* Returns clip bytes written (0 on error). Truncates to VOICE_MAX_FRAMES. */
size_t voice_encode(voice_codec_t *v, const int16_t *pcm, size_t nsamples, uint8_t *out, size_t cap);
/* Returns PCM samples written (0 on error / unknown format). */
size_t voice_decode(voice_codec_t *v, const uint8_t *clip, size_t len, int16_t *pcm, size_t cap_samples);

#endif
