// Native VP8 decode — the answer to "why does everything crawl when the video
// plays".
//
// MEASURED with `sample` on the guest thread while dolby.webm plays:
//
//     VP8 decode = 2722 of 3004 samples  =  91% of the guest's ONLY core
//        sub_8d5650  35.5%   sub_8da2b0  24.5%   sub_8d5b60  22.1%
//
// The port's scheduler is cooperative: every guest thread is a ucontext coroutine
// on ONE OS thread. So the decoder does not slow the video down — it starves the
// whole game, which is why the menu never finishes loading and the mixer
// stutters. Two factors multiply to produce that: lifted x86 runs several times
// slower than native, and libvpx is forced onto its SCALAR C kernels because
// remill cannot lift the SSE2 ones (VPX_SIMD_CAPS=0). Roughly 10-20x.
//
// Optimising lifted code cannot close a gap that big. NOT TRANSLATING IT AT ALL
// can: libvpx is a normal library, Homebrew ships an arm64 build with its NEON
// kernels, and this port already has the seam — `vpx_codec_decode` (FUN_008d1980)
// is interposed. Same move as vendoring HLSLcc for the shaders and
// reimplementing __libm_sse2_atan2 natively.
//
// THIS FILE IS STEP ONE AND ONLY STEP ONE: run a real arm64 libvpx alongside the
// guest's decoder, on exactly the same compressed frames, and report what it
// produces. That answers "can the native decoder read these bitstreams at all"
// with evidence before any of the guest's memory is touched. Handing the frames
// BACK to the game (vpx_codec_get_frame returning a guest-visible vpx_image_t)
// is step two, and it is the part that needs the guest's own struct layout —
// which is exactly what this probe is here to let us compare against.
//
// BO2_VPX_NATIVE=1 turns it on. Off by default: it costs a second decode.

#include <cstdint>
#include "../win32/bo2_getenv.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>

#define VPX_CODEC_DISABLE_COMPAT 1
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>

namespace {

vpx_codec_ctx_t g_ctx;
bool  g_ready = false, g_failed = false;
uint64_t g_frames = 0, g_bytes = 0, g_errors = 0;
// ⚠️ PER-DECODER frame counter, separate from the lifetime total.
//
// The frame log used to sample on g_frames alone (<=4 || %100), and g_frames is a
// LIFETIME counter across every video. The boot logos push it past 4, so a later
// video that decodes fewer than ~100 frames prints NOTHING — and this session read
// exactly that silence as "the campaign prologue is never decoded", made it the
// headline conclusion, and committed it. The tester, watching the screen, reported
// blue/black/white/pink flashes at that very moment: frames were being presented all
// along. Eighth capped view in this project, and the most expensive.
uint64_t g_frames_this_video = 0;
// The decoder is a singleton (one vpx_codec_ctx for the whole process), so there is
// no per-video init to hang the reset on. The file open is the honest boundary, and
// impl_CreateFileA calls this whenever the title opens a .webm.
extern "C" void bo2_vpx_new_video(void) { g_frames_this_video = 0; }
double   g_decode_us = 0.0;

bool ensure_init() {
    if (g_ready) return true;
    if (g_failed) return false;
    vpx_codec_dec_cfg_t cfg{};
    // WAS 1, deliberately: "one thread — we are measuring the codec, not the
    // machine". That was right while this was a measurement and wrong now that the
    // decoder is in the boot path. libvpx spawns REAL OS threads, so its work is the
    // one part of this port that does not have to queue behind the cooperative
    // scheduler's single guest core — and the boot is currently dominated by exactly
    // that contention: four minutes of video followed by an 83 MB zone inflate, all
    // serialised. Let the codec use the cores the machine actually has.
    unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2) hw = 2;
    if (hw > 4) hw = 4;                   // 4 is where VP8 tiling stops paying
    cfg.threads = hw;
    vpx_codec_err_t r = vpx_codec_dec_init(&g_ctx, vpx_codec_vp8_dx(), &cfg, 0);
    if (r != VPX_CODEC_OK) {
        fprintf(stderr, "[VPXN] native vpx_codec_dec_init failed: %s\n",
                vpx_codec_err_to_string(r));
        g_failed = true;
        return false;
    }
    fprintf(stderr, "[VPXN] native libvpx ready: %s\n", vpx_codec_version_str());
    g_ready = true;
    return true;
}

}  // namespace

// Step two: decode natively and DESCRIBE the frame, so the caller can write it
// into the guest's own vpx_image_t. The guest's layout was read off a frame it
// decoded itself (bo2_vp8_decode's [VP8IMG] dump) and agrees with the native one
// on every field that matters:
//     +0x00 fmt=258 (I420)  +0x0c d_w=1280  +0x10 d_h=720
//     +0x14/+0x18 chroma shift 1/1   +0x1c..+0x24 planes  +0x2c..+0x34 strides
// Native libvpx reported the same fmt and the same 1344/672/672 strides, so the
// handoff is three plane copies and no format conversion at all.
struct Bo2VpxFrame {
    const uint8_t* plane[3];
    int      stride[3];
    uint32_t d_w, d_h, fmt;
};
extern "C" int bo2_vpx_native_frame(Bo2VpxFrame* out);
static Bo2VpxFrame g_last{};
static bool        g_have_last = false;

extern "C" int bo2_vpx_native_frame(Bo2VpxFrame* out) {
    if (!g_have_last || !out) return 0;
    *out = g_last;
    return 1;
}

// Called from bo2_vpx_decode with the SAME buffer the guest decoder is about to
// be handed.
extern "C" void bo2_vpx_native_decode(const uint8_t* data, uint32_t size) {
    static const int on = bo2_env_on("BO2_VPX_NATIVE") ? 1 : 0;
    if (!on || !data || !size) return;
    if (!ensure_init()) return;

    const auto t0 = std::chrono::steady_clock::now();
    vpx_codec_err_t r = vpx_codec_decode(&g_ctx, data, size, nullptr, 0);
    const auto t1 = std::chrono::steady_clock::now();
    g_decode_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
    g_bytes += size;

    if (r != VPX_CODEC_OK) {
        if (g_errors++ < 8)
            fprintf(stderr, "[VPXN] decode #%llu FAILED on %u bytes: %s (%s)\n",
                    (unsigned long long)g_frames, size, vpx_codec_err_to_string(r),
                    vpx_codec_error_detail(&g_ctx) ? vpx_codec_error_detail(&g_ctx) : "-");
        return;
    }
    vpx_codec_iter_t it = nullptr;
    g_have_last = false;
    while (const vpx_image_t* img = vpx_codec_get_frame(&g_ctx, &it)) {
        g_frames++;
        g_frames_this_video++;
        for (int i = 0; i < 3; i++) {
            g_last.plane[i]  = img->planes[i];
            g_last.stride[i] = img->stride[i];
        }
        g_last.d_w = img->d_w; g_last.d_h = img->d_h; g_last.fmt = (uint32_t)img->fmt;
        g_have_last = true;
        // Report the PERCEPT, not the plumbing: a frame that decodes to a
        // constant is not a picture. Cheap luma spread over a sparse sample.
        uint8_t lo = 255, hi = 0;
        const uint8_t* y = img->planes[VPX_PLANE_Y];
        for (unsigned r2 = 0; r2 < img->d_h; r2 += 8) {
            const uint8_t* row = y + (size_t)r2 * img->stride[VPX_PLANE_Y];
            for (unsigned c = 0; c < img->d_w; c += 8) {
                if (row[c] < lo) lo = row[c];
                if (row[c] > hi) hi = row[c];
            }
        }
        if (g_frames_this_video <= 4 || (g_frames_this_video % 25) == 0)
            fprintf(stderr, "[VPXN] frame %llu (this video #%llu): %ux%u fmt=%d strides=%d/%d/%d "
                            "luma %u..%u | %llu KiB in, %.2f ms/frame avg\n",
                    (unsigned long long)g_frames, (unsigned long long)g_frames_this_video,
                    img->d_w, img->d_h, (int)img->fmt,
                    img->stride[0], img->stride[1], img->stride[2], lo, hi,
                    (unsigned long long)(g_bytes >> 10),
                    g_decode_us / 1000.0 / (double)g_frames);
    }
}
