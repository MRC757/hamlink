// SPDX-License-Identifier: GPL-3.0-or-later
package org.hamlink.app

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder

private const val RATE = 8000

private fun builtin(ctx: Context, input: Boolean): AudioDeviceInfo? {
    val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    val flag = if (input) AudioManager.GET_DEVICES_INPUTS else AudioManager.GET_DEVICES_OUTPUTS
    val type = if (input) AudioDeviceInfo.TYPE_BUILTIN_MIC else AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
    return am.getDevices(flag).firstOrNull { it.type == type }
}

/** Records the phone's own mic (NOT the radio link) at 8 kHz while the talk button is held. */
class VoiceRecorder(private val ctx: Context, private val maxSeconds: Int = 20) {
    private val buf = ShortArray(RATE * maxSeconds)
    private var n = 0
    @Volatile private var recording = false
    private var rec: AudioRecord? = null
    private var thread: Thread? = null

    fun start() {
        val min = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val r = AudioRecord.Builder()
            .setAudioSource(MediaRecorder.AudioSource.VOICE_RECOGNITION)
            .setAudioFormat(
                AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(RATE).setChannelMask(AudioFormat.CHANNEL_IN_MONO).build()
            )
            .setBufferSizeInBytes(maxOf(min, RATE))
            .build()
        builtin(ctx, true)?.let { r.setPreferredDevice(it) } // not the USB radio interface
        rec = r
        n = 0
        recording = true
        r.startRecording()
        thread = Thread {
            while (recording && n < buf.size) {
                val got = r.read(buf, n, minOf(1600, buf.size - n))
                if (got <= 0) break
                n += got
            }
        }.also { it.start() }
    }

    fun stop(): ShortArray {
        recording = false
        thread?.join(500)
        try { rec?.stop() } catch (_: Exception) {}
        rec?.release()
        rec = null
        return buf.copyOf(n)
    }
}

object VoicePlayer {
    fun play(ctx: Context, pcm: ShortArray) {
        Thread {
            val min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
            val t = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build()
                )
                .setAudioFormat(
                    AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(RATE).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build()
                )
                .setBufferSizeInBytes(maxOf(min, RATE))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            builtin(ctx, false)?.let { t.setPreferredDevice(it) } // phone speaker, never the radio
            try {
                t.play()
                var off = 0
                while (off < pcm.size) {
                    val w = t.write(pcm, off, pcm.size - off)
                    if (w < 0) break
                    off += w
                }
                val deadline = System.currentTimeMillis() + 2000
                while (t.playbackHeadPosition < pcm.size && System.currentTimeMillis() < deadline) Thread.sleep(20)
            } finally {
                t.stop()
                t.release()
            }
        }.start()
    }
}
