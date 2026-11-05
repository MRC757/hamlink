/* SPDX-License-Identifier: GPL-3.0-or-later */
/* JNI glue between org.hamlink.app.Native and the flat C API (core/hamlink_api.h).
 * All calls must come from ONE engine thread (the core is not thread safe); listener callbacks
 * are invoked synchronously on that same thread using the JNIEnv of the active native call. */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hamlink_api.h"

static struct {
    jobject listener;
    jmethodID m_msg, m_status, m_event, m_monitor;
    hl_ctx *ctx;
    JNIEnv *env; /* valid only inside a native call */
} G;

static void HL_CALL cb_message(void *u, const char *src, const char *dst, int type, const uint8_t *data, int len) {
    (void)u;
    JNIEnv *env = G.env;
    if (!env || !G.listener) return;
    jstring s = (*env)->NewStringUTF(env, src); /* sanitised to printable ASCII by the core */
    jstring d = (*env)->NewStringUTF(env, dst);
    jbyteArray arr = (*env)->NewByteArray(env, len);
    (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)data);
    (*env)->CallVoidMethod(env, G.listener, G.m_msg, s, d, (jint)type, arr);
    (*env)->DeleteLocalRef(env, s);
    (*env)->DeleteLocalRef(env, d);
    (*env)->DeleteLocalRef(env, arr);
}

static void HL_CALL cb_monitor(void *u, const char *src, const char *dst, int type, int msg_id, const uint8_t *data,
                               int len) {
    (void)u;
    JNIEnv *env = G.env;
    if (!env || !G.listener) return;
    jstring s = (*env)->NewStringUTF(env, src);
    jstring d = (*env)->NewStringUTF(env, dst);
    jbyteArray arr = (*env)->NewByteArray(env, len);
    (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)data);
    (*env)->CallVoidMethod(env, G.listener, G.m_monitor, s, d, (jint)type, (jint)msg_id, arr);
    (*env)->DeleteLocalRef(env, s);
    (*env)->DeleteLocalRef(env, d);
    (*env)->DeleteLocalRef(env, arr);
}

static void HL_CALL cb_status(void *u, int id, int status) {
    (void)u;
    if (!G.env || !G.listener) return;
    (*G.env)->CallVoidMethod(G.env, G.listener, G.m_status, (jint)id, (jint)status);
}

static void HL_CALL cb_event(void *u, const char *text) {
    (void)u;
    JNIEnv *env = G.env;
    if (!env || !G.listener) return;
    jstring t = (*env)->NewStringUTF(env, text);
    (*env)->CallVoidMethod(env, G.listener, G.m_event, t);
    (*env)->DeleteLocalRef(env, t);
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_start(JNIEnv *env, jobject thiz, jstring callsign,
                                                         jboolean relay, jint ttl, jint preamble_ms,
                                                         jint seed, jobject listener) {
    (void)thiz;
    if (G.ctx) return;
    const char *cs = (*env)->GetStringUTFChars(env, callsign, NULL);
    G.listener = (*env)->NewGlobalRef(env, listener);
    jclass cls = (*env)->GetObjectClass(env, listener);
    G.m_msg = (*env)->GetMethodID(env, cls, "onMessage", "(Ljava/lang/String;Ljava/lang/String;I[B)V");
    G.m_status = (*env)->GetMethodID(env, cls, "onStatus", "(II)V");
    G.m_event = (*env)->GetMethodID(env, cls, "onEvent", "(Ljava/lang/String;)V");
    G.m_monitor = (*env)->GetMethodID(env, cls, "onMonitor", "(Ljava/lang/String;Ljava/lang/String;II[B)V");
    G.ctx = hl_create(cs, relay ? 1 : 0, ttl, preamble_ms, (uint32_t)seed, cb_message, cb_status, cb_event, NULL);
    if (G.ctx) hl_set_monitor_callback(G.ctx, cb_monitor, NULL);
    (*env)->ReleaseStringUTFChars(env, callsign, cs);
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_stop(JNIEnv *env, jobject thiz) {
    (void)thiz;
    hl_destroy(G.ctx);
    G.ctx = NULL;
    if (G.listener) (*env)->DeleteGlobalRef(env, G.listener);
    G.listener = NULL;
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setMonitor(JNIEnv *env, jobject thiz, jboolean on) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_monitor(G.ctx, on ? 1 : 0);
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setListenAll(JNIEnv *env, jobject thiz, jboolean on) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_listen_all(G.ctx, on ? 1 : 0);
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setStationId(JNIEnv *env, jobject thiz, jboolean on) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_station_id(G.ctx, on ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_org_hamlink_app_Native_rxModem(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return G.ctx ? hl_rx_modem(G.ctx) : -1;
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setRelay(JNIEnv *env, jobject thiz, jboolean on) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_relay(G.ctx, on ? 1 : 0);
}

/* Returns 0, or -1 if the OFDM modem could not start (the AFSK modem stays selected). */
JNIEXPORT jint JNICALL Java_org_hamlink_app_Native_setModem(JNIEnv *env, jobject thiz, jint modem) {
    (void)env; (void)thiz;
    return G.ctx ? hl_set_modem(G.ctx, modem) : -1;
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setLink(JNIEnv *env, jobject thiz, jint link, jint tnc_baud) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_link(G.ctx, link, tnc_baud);
}

JNIEXPORT jfloat JNICALL Java_org_hamlink_app_Native_processAudio(JNIEnv *env, jobject thiz, jshortArray pcm,
                                                                  jint n, jlong now, jfloat busy_thresh) {
    (void)thiz;
    if (!G.ctx) return 0.0f;
    G.env = env;
    jshort *p = (*env)->GetShortArrayElements(env, pcm, NULL);
    float level = hl_process_audio(G.ctx, (const int16_t *)p, n, (int64_t)now, busy_thresh);
    (*env)->ReleaseShortArrayElements(env, pcm, p, JNI_ABORT);
    G.env = NULL;
    return level;
}

JNIEXPORT jint JNICALL Java_org_hamlink_app_Native_send(JNIEnv *env, jobject thiz, jstring dst, jint type,
                                                        jbyteArray data, jboolean ack, jlong now) {
    (void)thiz;
    if (!G.ctx) return -1;
    G.env = env;
    const char *d = (*env)->GetStringUTFChars(env, dst, NULL);
    jsize n = (*env)->GetArrayLength(env, data);
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);
    int rc = hl_send(G.ctx, d, type, (const uint8_t *)bytes, n, ack ? 1 : 0, (int64_t)now);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
    (*env)->ReleaseStringUTFChars(env, dst, d);
    G.env = NULL;
    return rc;
}

JNIEXPORT jshortArray JNICALL Java_org_hamlink_app_Native_pollTx(JNIEnv *env, jobject thiz, jlong now, jfloat amp) {
    (void)thiz;
    if (!G.ctx) return NULL;
    G.env = env;
    int n = 0;
    const int16_t *audio = hl_poll_tx(G.ctx, (int64_t)now, amp, &n);
    G.env = NULL;
    if (!audio || n <= 0) return NULL;
    jshortArray out = (*env)->NewShortArray(env, n);
    (*env)->SetShortArrayRegion(env, out, 0, n, (const jshort *)audio);
    return out;
}

/* TNC link: feed bytes read from the Bluetooth/serial KISS stream (n may be 0 to run timers). */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_tncRx(JNIEnv *env, jobject thiz, jbyteArray bytes, jint n, jlong now) {
    (void)thiz;
    if (!G.ctx) return;
    G.env = env;
    jbyte *b = n > 0 ? (*env)->GetByteArrayElements(env, bytes, NULL) : NULL;
    hl_tnc_rx(G.ctx, (const uint8_t *)b, n, (int64_t)now);
    if (b) (*env)->ReleaseByteArrayElements(env, bytes, b, JNI_ABORT);
    G.env = NULL;
}

/* TNC link: KISS bytes to write to the radio now, or null. */
JNIEXPORT jbyteArray JNICALL Java_org_hamlink_app_Native_tncPollTx(JNIEnv *env, jobject thiz, jlong now) {
    (void)thiz;
    if (!G.ctx) return NULL;
    G.env = env;
    int n = 0;
    const uint8_t *p = hl_tnc_poll_tx(G.ctx, (int64_t)now, &n);
    G.env = NULL;
    if (!p || n <= 0) return NULL;
    jbyteArray out = (*env)->NewByteArray(env, n);
    (*env)->SetByteArrayRegion(env, out, 0, n, (const jbyte *)p);
    return out;
}

JNIEXPORT jlong JNICALL Java_org_hamlink_app_Native_nextWakeup(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return G.ctx ? (jlong)hl_next_wakeup(G.ctx) : 0;
}

/* out8 = {rx frames, dups, tx frames, relayed, suppressed, retries, ofdm ok, ofdm failed} */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_getStats(JNIEnv *env, jobject thiz, jintArray out8) {
    (void)thiz;
    if (!G.ctx || (*env)->GetArrayLength(env, out8) < 8) return;
    uint32_t v[8];
    hl_get_stats(G.ctx, v);
    jint j[8];
    for (int i = 0; i < 8; i++) j[i] = (jint)v[i];
    (*env)->SetIntArrayRegion(env, out8, 0, 8, j);
}

JNIEXPORT jbyteArray JNICALL Java_org_hamlink_app_Native_voiceEncode(JNIEnv *env, jobject thiz, jshortArray pcm, jint n) {
    (void)thiz;
    if (!G.ctx) return NULL;
    jshort *p = (*env)->GetShortArrayElements(env, pcm, NULL);
    uint8_t out[3200];
    int k = hl_voice_encode(G.ctx, (const int16_t *)p, n, out, (int)sizeof out);
    (*env)->ReleaseShortArrayElements(env, pcm, p, JNI_ABORT);
    if (k <= 0) return NULL;
    jbyteArray arr = (*env)->NewByteArray(env, k);
    (*env)->SetByteArrayRegion(env, arr, 0, k, (const jbyte *)out);
    return arr;
}

static jshortArray decode_clip(JNIEnv *env, hl_ctx *ctx, jbyteArray clip);

JNIEXPORT jshortArray JNICALL Java_org_hamlink_app_Native_voiceDecode(JNIEnv *env, jobject thiz, jbyteArray clip) {
    (void)thiz;
    if (!G.ctx) return NULL;
    return decode_clip(env, G.ctx, clip);
}

/* Stored clips (the Stations page) are played from the UI thread, whether or not the radio runs: they get their own
 * context, so they never touch the engine's, which only the engine thread may use. */
JNIEXPORT jshortArray JNICALL Java_org_hamlink_app_Native_storedVoiceDecode(JNIEnv *env, jobject thiz, jbyteArray clip) {
    (void)thiz;
    static hl_ctx *dec;
    if (!dec) dec = hl_create("N0CALL", 0, 1, 300, 1, NULL, NULL, NULL, NULL);
    if (!dec) return NULL;
    return decode_clip(env, dec, clip);
}

static jshortArray decode_clip(JNIEnv *env, hl_ctx *ctx, jbyteArray clip) {
    jsize len = (*env)->GetArrayLength(env, clip);
    jbyte *c = (*env)->GetByteArrayElements(env, clip, NULL);
    int cap = ((len / 4) + 1) * 320;
    int16_t *pcm = (int16_t *)malloc((size_t)cap * sizeof(int16_t));
    jshortArray out = NULL;
    if (pcm) {
        int k = hl_voice_decode(ctx, (const uint8_t *)c, len, pcm, cap);
        if (k > 0) {
            out = (*env)->NewShortArray(env, k);
            (*env)->SetShortArrayRegion(env, out, 0, k, (const jshort *)pcm);
        }
        free(pcm);
    }
    (*env)->ReleaseByteArrayElements(env, clip, c, JNI_ABORT);
    return out;
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_toneStart(JNIEnv *env, jobject thiz, jint mode) {
    (void)env; (void)thiz;
    if (G.ctx) hl_tone_start(G.ctx, mode);
}

/* Fills `out` (48 kHz) toward amplitude amp; returns the amplitude reached (0 once faded out). */
JNIEXPORT jfloat JNICALL Java_org_hamlink_app_Native_toneFill(JNIEnv *env, jobject thiz, jshortArray out, jint n, jfloat amp) {
    (void)thiz;
    if (!G.ctx) return 0.0f;
    jsize cap = (*env)->GetArrayLength(env, out);
    if (n <= 0 || n > cap || n > 4800) return 0.0f;
    int16_t tmp[4800];
    float reached = hl_tone_fill(G.ctx, tmp, n, amp);
    (*env)->SetShortArrayRegion(env, out, 0, n, (const jshort *)tmp);
    return reached;
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_noteExternalTx(JNIEnv *env, jobject thiz, jlong now, jint duration_ms) {
    (void)env; (void)thiz;
    if (G.ctx) hl_note_external_tx(G.ctx, (int64_t)now, duration_ms);
}

/* out5 = {rms dBFS, peak dBFS, 1200 Hz dBFS, 2200 Hz dBFS, clipped samples in last 100 ms} */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_getMeter(JNIEnv *env, jobject thiz, jfloatArray out5) {
    (void)thiz;
    if (!G.ctx || (*env)->GetArrayLength(env, out5) < 5) return;
    jfloat v[5];
    hl_get_meter(G.ctx, v);
    (*env)->SetFloatArrayRegion(env, out5, 0, 5, v);
}

/* Manual PTT: a human keys the radio, so ACK/retry timers are stretched. */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setManualPtt(JNIEnv *env, jobject thiz, jboolean on) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_manual_ptt(G.ctx, on ? 1 : 0);
}

/* Frames queued for transmission (ACKs, relays, IDs, fragments), due or not. */
JNIEXPORT jint JNICALL Java_org_hamlink_app_Native_pendingTx(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return G.ctx ? hl_pending_tx(G.ctx) : 0;
}

/* ---------------- reports, map, geo (API v5) ---------------- */

static jstring utf(JNIEnv *env, const char *s) { return (*env)->NewStringUTF(env, s ? s : ""); }

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setClock(JNIEnv *env, jobject thiz, jlong unix_s, jlong now_ms) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_clock(G.ctx, (int64_t)unix_s, (int64_t)now_ms);
}

JNIEXPORT jint JNICALL Java_org_hamlink_app_Native_schemaCount(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return hl_schema_count();
}

JNIEXPORT jstring JNICALL Java_org_hamlink_app_Native_schemaJson(JNIEnv *env, jobject thiz, jint index) {
    (void)thiz;
    const char *j = hl_schema_json(index);
    return j ? utf(env, j) : NULL;
}

/* Returns "OK:<msgId>" or "ERR:<reason>". */
JNIEXPORT jstring JNICALL Java_org_hamlink_app_Native_reportSend(JNIEnv *env, jobject thiz, jstring dst, jstring schema,
                                                                 jstring kv, jboolean ack, jlong now_ms) {
    (void)thiz;
    if (!G.ctx) return utf(env, "ERR:radio is not running");
    const char *d = (*env)->GetStringUTFChars(env, dst, NULL);
    const char *s = (*env)->GetStringUTFChars(env, schema, NULL);
    const char *k = (*env)->GetStringUTFChars(env, kv, NULL);
    char err[200] = "", out[260];
    int id = hl_report_send(G.ctx, d, s, k, ack ? 1 : 0, (int64_t)now_ms, err, (int)sizeof err);
    if (id >= 0) snprintf(out, sizeof out, "OK:%d", id);
    else if (err[0]) snprintf(out, sizeof out, "ERR:%s", err);
    else snprintf(out, sizeof out, "ERR:%s", id == -2 ? "send queue full, wait for pending messages" : "could not send");
    (*env)->ReleaseStringUTFChars(env, dst, d);
    (*env)->ReleaseStringUTFChars(env, schema, s);
    (*env)->ReleaseStringUTFChars(env, kv, k);
    return utf(env, out);
}

JNIEXPORT jstring JNICALL Java_org_hamlink_app_Native_reportSummary(JNIEnv *env, jobject thiz, jbyteArray data) {
    (void)thiz;
    jsize n = (*env)->GetArrayLength(env, data);
    jbyte *p = (*env)->GetByteArrayElements(env, data, NULL);
    char out[240];
    int k = hl_report_summary((const uint8_t *)p, n, out, (int)sizeof out);
    (*env)->ReleaseByteArrayElements(env, data, p, JNI_ABORT);
    return k > 0 ? utf(env, out) : NULL;
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_mapSetSelf(JNIEnv *env, jobject thiz, jdouble lat, jdouble lon,
                                                              jdouble radius_m, jlong now_ms) {
    (void)env; (void)thiz;
    if (G.ctx) hl_map_set_self_position(G.ctx, lat, lon, radius_m, (int64_t)now_ms);
}

JNIEXPORT jstring JNICALL Java_org_hamlink_app_Native_mapSnapshot(JNIEnv *env, jobject thiz, jlong now_ms) {
    (void)thiz;
    if (!G.ctx) return utf(env, "");
    static char buf[32768];
    hl_map_snapshot(G.ctx, (int64_t)now_ms, buf, (int)sizeof buf);
    return utf(env, buf);
}

JNIEXPORT jstring JNICALL Java_org_hamlink_app_Native_mapDetail(JNIEnv *env, jobject thiz, jstring key, jlong now_ms) {
    (void)thiz;
    if (!G.ctx) return NULL;
    const char *k = (*env)->GetStringUTFChars(env, key, NULL);
    char buf[2048];
    int n = hl_map_detail(G.ctx, k, (int64_t)now_ms, buf, (int)sizeof buf);
    (*env)->ReleaseStringUTFChars(env, key, k);
    return n > 0 ? utf(env, buf) : NULL;
}

JNIEXPORT void JNICALL Java_org_hamlink_app_Native_mapClear(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    if (G.ctx) hl_map_clear(G.ctx);
}

/* Geo helpers are stateless and safe to call from any thread. Returns {lat, lon, radius_m, kind} or null. */
JNIEXPORT jdoubleArray JNICALL Java_org_hamlink_app_Native_geoParse(JNIEnv *env, jobject thiz, jstring text) {
    (void)thiz;
    const char *t = (*env)->GetStringUTFChars(env, text, NULL);
    double lat, lon, rad;
    int kind;
    int ok = hl_geo_parse(t, &lat, &lon, &rad, &kind);
    (*env)->ReleaseStringUTFChars(env, text, t);
    if (!ok) return NULL;
    jdouble v[4] = { lat, lon, rad, (double)kind };
    jdoubleArray a = (*env)->NewDoubleArray(env, 4);
    (*env)->SetDoubleArrayRegion(env, a, 0, 4, v);
    return a;
}

JNIEXPORT jstring JNICALL Java_org_hamlink_app_Native_geoFormat(JNIEnv *env, jobject thiz, jdouble lat, jdouble lon, jint fmt) {
    (void)thiz;
    char out[64];
    return hl_geo_format(lat, lon, fmt, out, (int)sizeof out) > 0 ? utf(env, out) : utf(env, "");
}

/* Extra copies of each broadcast report we originate (0 = off). */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setReportRepeat(JNIEnv *env, jobject thiz, jint extra, jint interval_ms) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_report_repeat(G.ctx, extra, interval_ms);
}

/* Receive DSP options (API v7). mode: 0 off, 1 data only, 2 data or loud audio (default). */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setCarrierSense(JNIEnv *env, jobject thiz, jint mode) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_carrier_sense(G.ctx, mode);
}
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_setBitRepair(JNIEnv *env, jobject thiz, jboolean on) {
    (void)env; (void)thiz;
    if (G.ctx) hl_set_bit_repair(G.ctx, on ? 1 : 0);
}
/* FX.25 on transmitted AFSK frames (API v8): 0 off, 16/32/64 check bytes. Returns 0, or -1 for another value. */
JNIEXPORT jint JNICALL Java_org_hamlink_app_Native_setFec(JNIEnv *env, jobject thiz, jint check_bytes) {
    (void)env; (void)thiz;
    return G.ctx ? hl_set_fec(G.ctx, check_bytes) : -1;
}
/* out3 = {FX.25 blocks decoded, frames only FEC recovered, bytes corrected} */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_getFecInfo(JNIEnv *env, jobject thiz, jintArray out3) {
    (void)thiz;
    if (!G.ctx || (*env)->GetArrayLength(env, out3) < 3) return;
    uint32_t v[3];
    hl_get_fec_info(G.ctx, v);
    jint j[3] = {(jint)v[0], (jint)v[1], (jint)v[2]};
    (*env)->SetIntArrayRegion(env, out3, 0, 3, j);
}
JNIEXPORT jboolean JNICALL Java_org_hamlink_app_Native_channelBusy(JNIEnv *env, jobject thiz) {
    (void)env; (void)thiz;
    return G.ctx && hl_channel_busy(G.ctx) ? JNI_TRUE : JNI_FALSE;
}
/* out6 = {data detected now, AFSK frames decoded, repaired, repairs rejected (ambiguous), repairs rejected (structure), busy} */
JNIEXPORT void JNICALL Java_org_hamlink_app_Native_getRxInfo(JNIEnv *env, jobject thiz, jintArray out6) {
    (void)thiz;
    if (!G.ctx || (*env)->GetArrayLength(env, out6) < 6) return;
    uint32_t v[6];
    hl_get_rx_info(G.ctx, v);
    jint j[6];
    for (int i = 0; i < 6; i++) j[i] = (jint)v[i];
    (*env)->SetIntArrayRegion(env, out6, 0, 6, j);
}
