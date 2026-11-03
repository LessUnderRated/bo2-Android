// coreaudio_out.cpp — turn the guest's XAudio2 PCM into actual sound.
//
// The engine's audio path ends at IXAudio2SourceVoice::SubmitSourceBuffer: it
// hands over a block of PCM and expects the device to consume it over time.
// Everything upstream of that is BO2's own code (mixer, decoders, 3D panning),
// so nothing here needs to understand sound — it only has to be a device that
// drains buffers at the right rate. If it drained faster or slower than
// real time the engine's mixer would drift, so an AudioQueue (which pulls at
// exactly the sample rate) is the honest primitive rather than a timer.
//
// Deliberately a plain ring buffer with a mutex, not a lock-free queue: the
// producer is one guest thread, the consumer is CoreAudio's callback thread, and
// at 48 kHz stereo the contention is a few hundred microseconds a second. A
// clever structure here would buy nothing and could hide a real ordering bug.
#include <AudioToolbox/AudioToolbox.h>
#include "../win32/bo2_getenv.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kNumBuffers = 3;
constexpr int kFrames     = 1024;          // per AudioQueue buffer

std::mutex            g_mu;
std::vector<uint8_t>  g_ring;              // PCM waiting to be played
size_t                g_ring_rd = 0;
AudioQueueRef         g_queue   = nullptr;
AudioQueueBufferRef   g_bufs[kNumBuffers] = {};
int      g_channels = 2, g_rate = 48000, g_bits = 16;
bool     g_started  = false;
uint64_t g_submitted = 0, g_played = 0, g_starved = 0, g_dropped = 0;
// Peak sample magnitude of everything the engine has handed us. A pipeline can
// run at exactly real time and still be carrying pure silence — measured, it
// was: 32 s of submissions with a peak of 0. Transport health says nothing
// about audibility, so measure audibility separately and never infer it.
float    g_peak = 0.0f;
// Per-channel peaks. The first non-zero PCM this port ever produced measured
// 0.0001 overall while the stereo downmix stayed at exactly 0 — which happens
// if the only signal sits in a channel the downmix drops (LFE). "Where is the
// signal?" is a different question from "is there signal", and a single
// aggregate cannot answer it.
float    g_peak_ch[8] = {};

size_t frame_bytes() { return (size_t)g_channels * (size_t)(g_bits / 8); }

// AudioQueue pulls here on its own thread. Underrun fills with silence rather
// than repeating stale audio: a gap is honest, a loop sounds like a bug.
// The ring side of a buffer completion, shared by the real device callback and
// the silent fallback below. `dst` may be null (nothing to copy the PCM into) —
// the accounting is what the engine's clock reads, not the bytes.
uint32_t drain_locked(uint8_t* dst, uint32_t want) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t avail = g_ring.size() - g_ring_rd;
    uint32_t got = (uint32_t)(avail < want ? avail : want);
    if (got && dst) memcpy(dst, g_ring.data() + g_ring_rd, got);
    g_ring_rd += got;
    // Compact once the consumed prefix dominates, so the ring cannot grow
    // without bound during a long run.
    if (g_ring_rd > (1u << 20) && g_ring_rd * 2 > g_ring.size()) {
        g_ring.erase(g_ring.begin(), g_ring.begin() + (long)g_ring_rd);
        g_ring_rd = 0;
    }
    g_played += got;
    if (got < want) g_starved++;
    return got;
}

// BO2_MUTE=1 — SILENCE WITHOUT CHANGING ANYTHING ELSE.
//
// This exists because BO2_FORCE_AUDIO_FALLBACK is a bad mute BY CONSTRUCTION,
// whatever it turns out to break. It was documented as "cuts the sound without
// changing the behaviour of the game"; it does not silence the device, it
// REPLACES the device with a wall-time-paced thread, and the engine's clock IS
// the audio buffer-completion count. A flag that swaps the clock is not a mute,
// even when the game survives it.
//
// ⚠️ WHAT IT DOES *NOT* BREAK, measured: keyboard input. I suspected it, because
// three consecutive runs under the flag were black-screened with dead input —
// and then run #5, same flag, answered Press Start on the third press. The dead
// input was FOCUS (see [FOCUS] / BO2_FORCE_FOCUS in metal_backend.mm), and this
// comment previously said otherwise on the strength of a correlation with n=3
// and no control. Corrected before it was committed, which is the only reason
// it is not a wrong claim in the tree.
//
// What remains open: every run under this flag has had a BLACK frontend while
// the title bar reports 3300+ draws and 230 pipelines, and the last known-good
// render ran on "[AUDIO] CoreAudio output started: 6 ch, 48000 Hz, 32-bit
// float". That is still only a correlation, and it is the reason BO2_MUTE
// exists: it makes "quiet" and "fallback clock" separable, so the black screen
// can be tested against the real device without waking the tester's speakers.
//
// BO2_MUTE keeps the real device, the real callbacks, the real pacing and the
// real ring accounting — g_played still advances by exactly `got` — and only
// zeroes the bytes that would have reached the speakers. It is the mute a person
// asking for quiet actually wants, and it is behaviour-neutral by construction.
// Value-sensitive, not presence-sensitive: campaign_run.sh now passes BO2_MUTE=1
// by default, so `BO2_MUTE=0` has to be a real way to turn the sound back on.
static const int g_mute = bo2_getenv("BO2_MUTE") ? atoi(bo2_getenv("BO2_MUTE")) : 0;

void queue_cb(void*, AudioQueueRef q, AudioQueueBufferRef buf) {
    const uint32_t want = buf->mAudioDataBytesCapacity;
    uint32_t got = drain_locked((uint8_t*)buf->mAudioData, want);
    if (got < want) memset((uint8_t*)buf->mAudioData + got, 0, want - got);
    // AFTER the drain, so the ring is consumed at exactly the same rate and the
    // clock the engine reads is untouched. Zeroing here costs one memset per
    // buffer and nothing else.
    if (g_mute) memset(buf->mAudioData, 0, want);
    buf->mAudioDataByteSize = want;
    AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
    // HEARTBEAT ON THE CONSUMER SIDE. The submit log only fires when the guest
    // pushes, so when the guest stops pushing it says nothing at all about
    // whether the device is draining — "played=0" in that log was consistent
    // both with a dead queue and with a queue that simply had not run yet.
    if (bo2_getenv("BO2_AUDIO_LOG")) {
        static uint64_t cb = 0;
        if (++cb <= 4 || (cb % 200) == 0)
            fprintf(stderr, "[AUDIO] queue cb #%llu got=%u/%u played=%llu starved=%llu\n",
                    (unsigned long long)cb, got, want,
                    (unsigned long long)g_played, (unsigned long long)g_starved);
    }
}

// --- the device is not the only thing that can stop the engine ---------------
// THE ENGINE'S CLOCK IS THE AUDIO BUFFER-COMPLETION COUNT (bo2_audio_take_
// completed divides g_played by the unit size). So when the output device
// refuses to start, the engine does not merely go quiet — it FREEZES. Measured
// 2026-08-01: every AudioQueue on this machine started returning -66681
// (kAudioQueueErr_CannotStart), including a plain 2ch/44.1k/16-bit one from a
// 30-line standalone test, so it was the AudioQueue subsystem and not our
// 6-channel format. The port sat there for 450 s without reaching the first
// logo video, in FOUR consecutive runs, and the first three of those were spent
// blaming code changes that turned out to be innocent.
//
// A machine with no working audio must still be able to run the game. This
// fallback drives the completion accounting from a host thread at EXACTLY the
// real-time buffer period, which is what a working device produces — no faster.
// That distinction is the whole point: the "catch-up credit" that let this
// clock run AHEAD of real time is what broke video playback for days, and
// pacing on wall time in place of a WORKING device broke it too. This paces on
// wall time only when the alternative is a clock that never ticks at all.
bool start_silent_fallback();

bool ensure_queue() {
    if (g_queue || g_started) return true;
    // BO2_FORCE_AUDIO_FALLBACK=1 — ❌ NO LONGER "ALSO THE MUTE SWITCH".
    // It was documented that way (commit 2f6db7b) on the reasoning that the
    // clock "keeps ticking at exactly the right rate, so the game behaves
    // identically". The rate is right; the CLOCK IS NOT THE SAME CLOCK, and this
    // file's own header explains why that matters. Use BO2_MUTE=1 for quiet.
    //
    // Its real purpose — take the silent paced clock even though a
    // real device is available. Added 2026-08-01 to make ONE question testable
    // on a working machine: the frontend room rendered under the fallback clock
    // (locked Mac) and is BLACK under the real device, with identical draw
    // counts. Without this flag that comparison needs a broken Mac, which is not
    // a control anybody can run twice.
    // ⚠️ THIS IS NOT A MUTE (2026-08-02). It replaces the output device with a
    // wall-time-paced thread, and the engine's clock IS the buffer-completion
    // count — so it changes the clock, which is the opposite of
    // behaviour-neutral. "BO2_FORCE_AUDIO_FALLBACK doubles as the MUTE switch"
    // is retracted: for silence use BO2_MUTE=1 (real device, real pacing, zeroed
    // bytes). This flag remains the right tool for TESTING the fallback clock.
    // It does not break keyboard input — measured, run #5 answered Press Start
    // under it; that was focus. Whether it is behind the black frontend is open.
    if (bo2_getenv("BO2_FORCE_AUDIO_FALLBACK")) {
        fprintf(stderr, "[AUDIO] BO2_FORCE_AUDIO_FALLBACK — skipping the real device.\n"
                        "[AUDIO] ⚠️  THIS CHANGES THE ENGINE CLOCK. It is not a mute; "
                        "use BO2_MUTE=1 for silence.\n");
        return start_silent_fallback();
    }
    AudioStreamBasicDescription f = {};
    f.mSampleRate       = g_rate;
    f.mFormatID         = kAudioFormatLinearPCM;
    // 32-bit XAudio2 audio is IEEE FLOAT, not signed integer. Declaring it as
    // integer produces a descriptor CoreAudio accepts at creation and then never
    // pulls from — the queue simply never calls back, which reads as "no sound"
    // rather than as a format error. BO2 asks for 6ch/48kHz/32-bit here.
    f.mFormatFlags      = (g_bits == 32 ? kAudioFormatFlagIsFloat
                                        : kLinearPCMFormatFlagIsSignedInteger) |
                          kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = (UInt32)g_channels;
    f.mBitsPerChannel   = (UInt32)g_bits;
    f.mFramesPerPacket  = 1;
    f.mBytesPerFrame    = (UInt32)frame_bytes();
    f.mBytesPerPacket   = f.mBytesPerFrame;
    if (AudioQueueNewOutput(&f, queue_cb, nullptr, nullptr, nullptr, 0,
                            &g_queue) != noErr || !g_queue) {
        fprintf(stderr, "[AUDIO] AudioQueueNewOutput failed (%dch %dHz %dbit)\n",
                g_channels, g_rate, g_bits);
        g_queue = nullptr;
        return false;
    }
    // P807 : sans disposition, CoreAudio prend 6 canaux pour 6 voies discrètes et n'envoie que FL/FR à une sortie
    // stéréo : le centre (voix des films, Woods) et les arrières étaient jetés. Sous Windows, XAudio2 replie la voix
    // 5.1 du jeu vers le maître stéréo qu'on lui annonce. Masque WAVE FL FR C LFE BL BR = bits CoreAudio identiques.
    if (g_channels == 6) {
        AudioChannelLayout lay = {};
        lay.mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelBitmap;
        lay.mChannelBitmap    = 0x3F;
        const OSStatus ls = AudioQueueSetProperty(g_queue, kAudioQueueProperty_ChannelLayout, &lay, sizeof(lay));
        fprintf(stderr, "[AUDIO-LAYOUT] 5.1 FL FR C LFE BL BR -> %s (%d)\n", ls == noErr ? "pose" : "REFUSE", (int)ls);
    }
    const UInt32 cap = (UInt32)(kFrames * frame_bytes());
    for (int i = 0; i < kNumBuffers; i++) {
        if (AudioQueueAllocateBuffer(g_queue, cap, &g_bufs[i]) != noErr) return false;
        memset(g_bufs[i]->mAudioData, 0, cap);
        g_bufs[i]->mAudioDataByteSize = cap;
        AudioQueueEnqueueBuffer(g_queue, g_bufs[i], 0, nullptr);
    }
    OSStatus st = AudioQueueStart(g_queue, nullptr);
    if (st != noErr) {                 // check it: a failed Start is silent
        fprintf(stderr, "[AUDIO] AudioQueueStart failed: %d\n", (int)st);
        AudioQueueDispose(g_queue, true);
        g_queue = nullptr;
        return start_silent_fallback();
    }
    g_started = true;
    fprintf(stderr, "[AUDIO] CoreAudio output started: %d ch, %d Hz, %d-bit %s\n",
            g_channels, g_rate, g_bits, g_bits == 32 ? "float" : "int");
    return true;
}

// The fallback pump. One host thread, absolute schedule (never `sleep(period)`
// in a loop — that accumulates the wakeup latency and would make the clock run
// SLOW, drifting the mixer exactly the way a bad device would).
std::thread g_silent_thr;
std::atomic<bool> g_silent_run{false};
bool start_silent_fallback() {
    if (g_silent_run.load()) return true;
    if (bo2_getenv("BO2_NO_AUDIO_FALLBACK")) {
        fprintf(stderr, "[AUDIO] no device and BO2_NO_AUDIO_FALLBACK set — "
                        "the engine clock will not tick\n");
        return false;
    }
    g_silent_run.store(true);
    g_started = true;
    fprintf(stderr, "[AUDIO] ⚠️  NO WORKING OUTPUT DEVICE — running SILENT, with the "
                    "buffer clock paced on wall time (%d ch, %d Hz, %d-bit). The game "
                    "runs; there is no sound. Set BO2_NO_AUDIO_FALLBACK=1 to refuse instead.\n",
            g_channels, g_rate, g_bits);
    g_silent_thr = std::thread([] {
        using namespace std::chrono;
        auto next = steady_clock::now();
        while (g_silent_run.load()) {
            const size_t fb = frame_bytes();
            const uint32_t want = (uint32_t)(kFrames * (fb ? fb : 4));
            drain_locked(nullptr, want);
            // Period from the CURRENT rate: bo2_audio_set_format can change it
            // after the first voice is created.
            const int rate = g_rate > 0 ? g_rate : 48000;
            next += nanoseconds((long long)kFrames * 1000000000LL / rate);
            std::this_thread::sleep_until(next);
        }
    });
    g_silent_thr.detach();
    return true;
}

// Called when the guest creates a source voice, with the WAVEFORMATEX it asked
// for. Changing format after the queue exists would need a teardown; BO2 uses
// one output format, so the first one wins and a mismatch is reported instead of
// silently resampling (which would be a lie about what we support).
extern "C" void bo2_audio_set_format(int channels, int rate, int bits) {
    if (channels <= 0 || rate <= 0 || (bits != 8 && bits != 16 && bits != 32)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_queue) {
        if (channels != g_channels || rate != g_rate || bits != g_bits)
            fprintf(stderr, "[AUDIO] voice wants %dch/%dHz/%dbit but the device is "
                            "%dch/%dHz/%dbit — not resampling\n",
                    channels, rate, bits, g_channels, g_rate, g_bits);
        return;
    }
    g_channels = channels; g_rate = rate; g_bits = bits;
}

// ── BO2_AUDIO_WAV=<path>: tap the PCM on its way to the device ──────────────
// The only way to show what the port actually sounds like — a screenshot cannot
// carry it, and macOS has no loopback capture without installing a driver. Taps
// the same buffers the device gets, downmixed to 16-bit stereo so the file plays
// anywhere. Off unless the variable is set; the header is patched at exit.
FILE*    g_wav      = nullptr;
uint32_t g_wav_data = 0;

void wav_finish() {
    if (!g_wav) return;
    uint32_t riff = 36 + g_wav_data;
    fseek(g_wav, 4, SEEK_SET);  fwrite(&riff, 4, 1, g_wav);
    fseek(g_wav, 40, SEEK_SET); fwrite(&g_wav_data, 4, 1, g_wav);
    fclose(g_wav); g_wav = nullptr;
    fprintf(stderr, "[AUDIO] wav: %u bytes of 16-bit stereo written\n", riff);
}

void wav_open() {
    const char* path = bo2_getenv("BO2_AUDIO_WAV");
    if (!path || g_wav) return;
    g_wav = fopen(path, "wb");
    if (!g_wav) return;
    const uint32_t rate = (uint32_t)g_rate, byterate = rate * 4;
    const uint16_t ch = 2, bits = 16, fmt = 1, align = 4;
    const uint32_t z = 0, sixteen = 16;
    fwrite("RIFF", 4, 1, g_wav); fwrite(&z, 4, 1, g_wav);
    fwrite("WAVEfmt ", 8, 1, g_wav); fwrite(&sixteen, 4, 1, g_wav);
    fwrite(&fmt, 2, 1, g_wav);  fwrite(&ch, 2, 1, g_wav);
    fwrite(&rate, 4, 1, g_wav); fwrite(&byterate, 4, 1, g_wav);
    fwrite(&align, 2, 1, g_wav); fwrite(&bits, 2, 1, g_wav);
    fwrite("data", 4, 1, g_wav); fwrite(&z, 4, 1, g_wav);
    atexit(wav_finish);
    fprintf(stderr, "[AUDIO] wav: capturing to %s (%d ch %d Hz %d-bit -> 2ch 16-bit)\n",
            path, g_channels, g_rate, g_bits);
}

// 5.1 order is FL FR C LFE BL BR; fold the centre in at -3 dB and drop LFE,
// which is the conventional downmix and keeps dialogue audible.
void wav_write(const void* pcm, uint32_t bytes) {
    if (!g_wav || g_bits != 32) return;
    const float* f = (const float*)pcm;
    const uint32_t frames = bytes / (uint32_t)frame_bytes();
    std::vector<int16_t> o(2 * (size_t)frames);   // un seul fwrite par tampon : on tient g_mu, que le rappel du périphérique attend
    for (uint32_t i = 0; i < frames; i++) {
        const float* fr = f + (size_t)i * g_channels;
        float c = g_channels > 2 ? fr[2] * 0.7071f : 0.0f;
        float l = fr[0] + c, r = (g_channels > 1 ? fr[1] : fr[0]) + c;
        if (g_channels > 4) { l += fr[4] * 0.7071f; r += fr[5] * 0.7071f; }
        o[2 * i]     = (int16_t)(l > 1 ? 32767 : l < -1 ? -32768 : l * 32767);
        o[2 * i + 1] = (int16_t)(r > 1 ? 32767 : r < -1 ? -32768 : r * 32767);
    }
    fwrite(o.data(), 4, frames, g_wav);
    g_wav_data += 4 * frames;
}

}  // namespace

extern "C" void bo2_audio_submit(const void* pcm, uint32_t bytes) {
    // COUNT THE DROPS. The engine raises its own outstanding-buffer counter for
    // every SubmitSourceBuffer it makes, whatever we do with the bytes — so a
    // submission we silently discard here is a completion that can never come
    // back, and six of those pin that counter at its ceiling of 6 and stop the
    // mixer for the rest of the run. If this is ever non-zero, that is the bug.
    if (!pcm || !bytes) { g_dropped++; return; }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        // Cap the backlog. If the engine outruns the device we would otherwise
        // grow forever and play audio that is seconds stale; dropping the oldest
        // keeps latency bounded and makes the overrun visible in the counter.
        // 250 ms, not 1 s. A full second of backlog IS a full second of audible
        // lag, and bounding exactly that is the point of the cap.
        //
        // ⚠️ THIS IS THE HALF OF 602a7eb THAT IS SAFE TO KEEP. That commit made two
        // changes here: this cap, and a "catch-up credit" that granted EXTRA buffer
        // completions. The completions are the engine's clock, so the credit made
        // the clock run ahead of real time and the video sequencer skipped videos
        // 2..N — bisected, and reverting it restored 2/2/2. Discarding stale audio
        // does not touch the clock at all: it only decides which samples reach the
        // device. So the lag fix comes back, the clock stays honest.
        const size_t kMaxBacklog = (size_t)(g_rate / 4) * frame_bytes();
        if (g_ring.size() - g_ring_rd > kMaxBacklog) {
            g_ring.erase(g_ring.begin(), g_ring.begin() + (long)g_ring_rd);
            g_ring_rd = 0;
            if (g_ring.size() > kMaxBacklog)
                g_ring.erase(g_ring.begin(),
                             g_ring.begin() + (long)(g_ring.size() - kMaxBacklog));
        }
        g_ring.insert(g_ring.end(), (const uint8_t*)pcm, (const uint8_t*)pcm + bytes);
        g_submitted += bytes;
        {   // peak of the raw buffer, before any downmix of ours can be blamed
            const uint32_t n = bytes / 4;
            if (g_bits == 32) {
                const float* f = (const float*)pcm;
                uint32_t nf = 0;
                for (uint32_t i = 0; i < n; i++) {
                    float v = f[i] < 0 ? -f[i] : f[i];
                    if (!(v <= 1e30f)) { nf++; continue; }   // NaN/Inf : invisibles pour `v > g_peak`
                    if (v > g_peak) g_peak = v;
                    const uint32_t c = (uint32_t)(i % (uint32_t)g_channels);
                    if (c < 8 && v > g_peak_ch[c]) g_peak_ch[c] = v;
                }
                // P942 : grésillement aigu entendu par le testeur (25/10, P940) sans trace : la crête ne voit pas un
                // NaN, et le WAV le rend en silence (fcvtzs(NaN) = 0). Un tampon non fini va tel quel à CoreAudio.
                if (nf) {
                    static uint64_t tot = 0, tampons = 0;
                    tot += nf;
                    if ((++tampons & (tampons - 1)) == 0)
                        fprintf(stderr, "[SON-NONFINI] tampon #%llu soumis=%llu : %u/%u echantillons NaN/Inf (cumul %llu)\n",
                                (unsigned long long)tampons, (unsigned long long)g_submitted, nf, n,
                                (unsigned long long)tot);
                }
            } else {
                const int16_t* q = (const int16_t*)pcm;
                for (uint32_t i = 0; i < bytes / 2; i++) {
                    float v = (q[i] < 0 ? -q[i] : q[i]) / 32768.0f;
                    if (v > g_peak) g_peak = v;
                }
            }
        }
        wav_open();
        wav_write(pcm, bytes);
    }
    ensure_queue();
    if (bo2_getenv("BO2_AUDIO_LOG")) {
        static uint64_t n = 0;
        if (++n <= 8 || (n % 500) == 0)
            fprintf(stderr, "[AUDIO] submit #%llu %u B (submitted=%llu played=%llu "
                            "starved=%llu)\n", (unsigned long long)n, bytes,
                    (unsigned long long)g_submitted, (unsigned long long)g_played,
                    (unsigned long long)g_starved);
    }
}

// XAUDIO2_VOICE_STATE, for IXAudio2SourceVoice::GetState. THE ENGINE THROTTLES
// ON THIS. XAudio2 normally reports progress two ways: an
// IXAudio2VoiceCallback::OnBufferEnd callback, and this poll. We cannot invoke
// the callback — it is guest code and the AudioQueue thread has no guest State,
// so entering it there would corrupt whichever thread's registers we borrowed.
// The poll is safe and sufficient: reporting real BuffersQueued/SamplesPlayed
// tells the mixer its buffers are draining, and it keeps feeding. Returning
// zeros made it submit 6 buffers (32 ms) and stop, because as far as it could
// tell nothing had ever been consumed.
// 🎯 THE ENGINE'S CLOCK, FOR THE HOST-CLOCKED [CADENCE] LINE.
//
// The header above this file says it outright: the engine's clock IS the audio
// buffer-completion count, and when the output device refuses to start the port
// does not go quiet, it FREEZES. So "is g_played still advancing" is the one
// number that separates a guest that is stuck from a guest that is waiting on a
// clock which stopped — and nothing in this port has ever plotted it against a
// freeze. g_starved answers the other direction: the device asking for data the
// guest is not producing means the GUEST is the stuck one.
//
// These live in an anonymous namespace (internal linkage) so [CADENCE] cannot
// reference them directly; this is the accessor rather than moving them out,
// because their internal linkage is what keeps the ring accounting private.
extern "C" void bo2_audio_clock(uint64_t* played, uint64_t* submitted, uint64_t* starved) {
    if (played)    *played    = g_played;
    if (submitted) *submitted = g_submitted;
    if (starved)   *starved   = g_starved;
}

extern "C" void bo2_audio_state(uint32_t* buffers_queued, uint64_t* samples_played) {
    std::lock_guard<std::mutex> lk(g_mu);
    size_t fb = frame_bytes();
    size_t pending = g_ring.size() - g_ring_rd;
    if (buffers_queued) {
        // The engine submits fixed-size blocks; report the backlog in units of
        // one submission so "queued" means what it means on Windows.
        size_t unit = 6144;
        *buffers_queued = (uint32_t)((pending + unit - 1) / unit);
    }
    if (samples_played) *samples_played = fb ? (g_played / fb) : 0;
}

// How many whole submissions the device has finished since the last ask, capped
// at `max`. The engine needs this to know a buffer is free again; see the
// OnBufferEnd note in remaining_dlls_impl.cpp for why it cannot be delivered
// from the audio thread.
//
// ⚠️ THE CAP IS NOT COSMETIC — IT IS WHAT MAKES THIS SAFE. This call CONSUMES
// credit: whatever it returns, the caller is committed to delivering, because
// `reported` has already moved. The first version took the whole backlog and
// then delivered `min(done, 256)` of it, so every buffer past the cap — and the
// entire batch whenever the callback pointer happened to read back as 0 — was
// silently thrown away. Six lost callbacks are enough to livelock the pipeline
// for good: the engine's queue count sticks at its ceiling of 6, so it submits
// nothing, so the device drains nothing, so no further completions arrive to
// unstick it. Observed exactly that: 1655 submissions, 1649 callbacks, audio
// frozen at 10 MB with 28,876 starved device callbacks and no way back.
// Ask for only as many as you will actually deliver.
extern "C" float bo2_audio_peak() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_peak;
}

extern "C" void bo2_audio_peaks(float* out8) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < 8; i++) out8[i] = g_peak_ch[i];
}

extern "C" void bo2_audio_counters(uint64_t* submitted, uint64_t* played,
                                   uint64_t* starved, uint64_t* dropped) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (submitted) *submitted = g_submitted;
    if (played)    *played    = g_played;
    if (starved)   *starved   = g_starved;
    if (dropped)   *dropped   = g_dropped;
}

extern "C" uint32_t bo2_audio_take_completed(uint32_t unit_bytes, uint32_t max) {
    if (!unit_bytes || !max) return 0;
    std::lock_guard<std::mutex> lk(g_mu);
    static uint64_t reported = 0;
    uint64_t done = g_played / unit_bytes;
    if (done <= reported) return 0;
    uint64_t n = done - reported;
    if (n > max) n = max;
    reported += n;
    return (uint32_t)n;
}
