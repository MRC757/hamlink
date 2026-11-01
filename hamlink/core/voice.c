/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "voice.h"
#include <stdlib.h>
#include <string.h>
#include "codec2.h"

struct voice_codec {
    struct CODEC2 *enc, *dec;
};

voice_codec_t *voice_create(void) {
    voice_codec_t *v = (voice_codec_t *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->enc = codec2_create(CODEC2_MODE_700C);
    v->dec = codec2_create(CODEC2_MODE_700C);
    if (!v->enc || !v->dec) { voice_destroy(v); return NULL; }
    return v;
}

void voice_destroy(voice_codec_t *v) {
    if (!v) return;
    if (v->enc) codec2_destroy(v->enc);
    if (v->dec) codec2_destroy(v->dec);
    free(v);
}

size_t voice_encode(voice_codec_t *v, const int16_t *pcm, size_t n, uint8_t *out, size_t cap) {
    size_t frames = (n + VOICE_FRAME_SAMPLES - 1) / VOICE_FRAME_SAMPLES;
    if (frames > VOICE_MAX_FRAMES) frames = VOICE_MAX_FRAMES;
    if (frames == 0 || cap < 1 + frames * VOICE_FRAME_BYTES) return 0;
    out[0] = VOICE_FMT_C2_700C;
    short block[VOICE_FRAME_SAMPLES];
    for (size_t f = 0; f < frames; f++) {
        size_t off = f * VOICE_FRAME_SAMPLES;
        size_t have = n > off ? n - off : 0;
        if (have > VOICE_FRAME_SAMPLES) have = VOICE_FRAME_SAMPLES;
        memset(block, 0, sizeof block);
        memcpy(block, pcm + off, have * sizeof(short));
        codec2_encode(v->enc, out + 1 + f * VOICE_FRAME_BYTES, block);
    }
    return 1 + frames * VOICE_FRAME_BYTES;
}

size_t voice_decode(voice_codec_t *v, const uint8_t *clip, size_t len, int16_t *pcm, size_t cap) {
    if (len < 1 + VOICE_FRAME_BYTES || clip[0] != VOICE_FMT_C2_700C) return 0;
    size_t frames = (len - 1) / VOICE_FRAME_BYTES;
    if (cap < frames * VOICE_FRAME_SAMPLES) return 0;
    for (size_t f = 0; f < frames; f++)
        codec2_decode(v->dec, pcm + f * VOICE_FRAME_SAMPLES, clip + 1 + f * VOICE_FRAME_BYTES);
    return frames * VOICE_FRAME_SAMPLES;
}
