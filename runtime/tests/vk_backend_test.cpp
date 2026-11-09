// vk_backend_test.cpp — the Vulkan backend driven as bo2-mac's runtime drives it, on the PC, headless.
//
// Our own shaders (runtime/tests/shaders/tri_*.hlsl, DXBC by make_dxbc.cpp) go through dxbc_to_vk.cpp and
// gfx_register_shader2 with their maps, as remaining_dlls_impl.cpp registers them; each scene is a frame of GfxDraw
// records through gfx_real_draw / gfx_real_frame_end / gfx_pump_once, and the back buffer is read back and checked.
//   vk_backend_test <shaders dir>
#include "../gfx/vk/gfx_abi.h"
#include "../gfx/vk/vk_hooks.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using bo2vk::GfxDraw;
using bo2vk::GfxDrawElem;

extern "C" {
// What the producer (remaining_dlls_impl.cpp) provides the backend.
uint32_t gfx_draw_record_size(void) { return (uint32_t)sizeof(GfxDraw); }
float gfx_depth_clear_value() { return 1.0f; }

int bo2_dxbc_translate2(const void*, uint32_t, char**, char**, char**, char**, char**);
char* bo2_msl_attributes(const char*);
void bo2_msl_free(char*);
bool gfx_window_open(int, int);
bool gfx_pump_once();
void gfx_shutdown();
void gfx_real_draw(const GfxDraw*);
void gfx_real_frame_end();
void gfx_register_shader2(uint32_t, int, const char*, const char*);
void gfx_register_texslots(uint32_t, const char*);
void gfx_register_cbslots(uint32_t, const char*);
void gfx_register_texdims(uint32_t, const char*);
void gfx_register_smpslots(uint32_t, const char*);
void gfx_register_sampler(uint32_t, const uint32_t[13]);
void gfx_register_texture(uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t, uint32_t);
void gfx_release_texture(uint32_t);
void gfx_register_rendertarget(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
void gfx_register_rtv(uint32_t, uint32_t, uint32_t);
void gfx_set_backbuffer_rtv(uint32_t);
void gfx_set_backbuffer_res(uint32_t);
int gfx_texture_dims(uint32_t, uint32_t*, uint32_t*);
uint32_t gfx_cbreg_for_mtl(uint32_t, uint32_t);
}

static int g_failed = 0;
#define CHECK(c)                                                                                \
    do {                                                                                        \
        if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_failed; } \
    } while (0)

static std::string read(const std::string& p) {
    std::string s;
    if (FILE* f = fopen(p.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        fclose(f);
    }
    return s;
}

// Translate and register a shader exactly as the runtime does (remaining_dlls_impl.cpp ~3168-3306).
static bool register_shader(const std::string& dir, const char* name, uint32_t obj, bool pixel) {
    const std::string dxbc = read(dir + "/" + name + ".dxbc");
    char *src = nullptr, *tex = nullptr, *cb = nullptr, *dim = nullptr, *smp = nullptr;
    if (dxbc.empty() || !bo2_dxbc_translate2(dxbc.data(), (uint32_t)dxbc.size(), &src, &tex, &cb, &dim, &smp) || !src)
        return false;
    char* attrs = bo2_msl_attributes(src);
    gfx_register_shader2(obj, pixel, src, attrs);
    if (tex) gfx_register_texslots(obj, tex);
    if (dim) gfx_register_texdims(obj, dim);
    if (smp && pixel) gfx_register_smpslots(obj, smp);
    if (cb) gfx_register_cbslots(obj, cb);
    for (char* p : {src, tex, cb, dim, smp, attrs}) bo2_msl_free(p);
    return true;
}

struct Vtx { float x, y, z; uint8_t r, g, b, a; float u, v; };
static const GfxDrawElem kElems[3] = {{"POSITION0", 6, 0, 0}, {"COLOR0", 28, 12, 0}, {"TEXCOORD0", 16, 16, 0}};

constexpr uint32_t kVS = 0x1001, kPS = 0x1002, kBBRtv = 0x9001, kBBRes = 0x9000, kSampler = 0x3001;
constexpr uint32_t W = 1920, H = 1080;   // the guest monitor (guest_monitor.h)

struct Scene {
    std::vector<std::vector<Vtx>> verts;   // kept alive until the frame is ended (gfx_real_draw copies)
    std::vector<uint16_t> quad_idx{0, 1, 2, 2, 1, 3};
    float xform[4] = {0, 0, 1, 1};
    struct Tint { float scalar; float misaligned[2]; float pad; float tint[4]; } tint{1, {1, 1}, 0, {1, 1, 1, 1}};
};

// A quad from (x0, y0) to (x1, y1) in NDC (y up, as D3D), at depth z, one colour, UVs 0..1 from the top left.
static std::vector<Vtx> quad(float x0, float y0, float x1, float y1, float z, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return {{x0, y1, z, r, g, b, a, 0, 0}, {x1, y1, z, r, g, b, a, 1, 0}, {x0, y0, z, r, g, b, a, 0, 1},
            {x1, y0, z, r, g, b, a, 1, 1}};
}

static GfxDraw draw_of(Scene& s, const std::vector<Vtx>& v, uint32_t rtv, uint32_t tex) {
    GfxDraw d;
    memset(&d, 0, sizeof d);
    d.vs = kVS; d.ps = kPS;
    d.elems = kElems; d.nelems = 3;
    d.vb = v.data(); d.vb_bytes = (uint32_t)(v.size() * sizeof(Vtx)); d.vb_stride = sizeof(Vtx);
    d.ib = s.quad_idx.data(); d.ib_bytes = (uint32_t)(s.quad_idx.size() * 2); d.ib_fmt = 57;
    d.idx_count = 6; d.topo = 4; d.rtv = rtv;
    const float vw = rtv == kBBRtv ? (float)W : 64.f, vh = rtv == kBBRtv ? (float)H : 64.f;
    d.vp[2] = vw; d.vp[3] = vh; d.vp[5] = 1.f;
    d.vscb[0] = s.xform; d.vscb_bytes[0] = sizeof s.xform;
    d.pscb[1] = &s.tint; d.pscb_bytes[1] = sizeof s.tint;
    d.pstex[2] = tex; d.pssamp[3] = kSampler;
    d.blend.write_mask = 0xF;
    d.depth.func = 2;
    return d;
}

static std::vector<uint8_t> g_px(W * H * 4);
static void frame() {
    gfx_real_frame_end();
    CHECK(gfx_pump_once());
    uint32_t w = 0, h = 0;
    CHECK(bo2vk_read_backbuffer(g_px.data(), (uint32_t)g_px.size(), &w, &h) && w == W && h == H);
}
static bool px_is(uint32_t x, uint32_t y, int r, int g, int b, const char* what) {
    const uint8_t* p = &g_px[((size_t)y * W + x) * 4];
    const bool ok = std::abs(p[0] - r) <= 3 && std::abs(p[1] - g) <= 3 && std::abs(p[2] - b) <= 3;
    if (!ok) fprintf(stderr, "  %s at (%u, %u): got %u,%u,%u, want %d,%d,%d\n", what, x, y, p[0], p[1], p[2], r, g, b);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) { fprintf(stderr, "vk_backend_test <shaders dir>\n"); return 2; }
    const std::string dir = argv[1];
    if (!gfx_window_open(1280, 720)) { fprintf(stderr, "no Vulkan 1.3 device: cannot run\n"); return 77; }
    gfx_set_backbuffer_rtv(kBBRtv);
    gfx_set_backbuffer_res(kBBRes);
    CHECK(register_shader(dir, "tri_vs", kVS, false));
    CHECK(register_shader(dir, "tri_ps", kPS, true));
    const uint32_t point_clamp[13] = {0, 3, 3, 3, 0, 1, 0, 0, 0, 0, 0, 0, 0x7f7fffff};   // MIN_MAG_MIP_POINT
    gfx_register_sampler(kSampler, point_clamp);
    // Textures: 2x2 (red, green / blue, white), 1x1 white, a BC1 4x4 block of red.
    const uint8_t quad_tex[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    gfx_register_texture(0x2001, 2, 2, 28, quad_tex, sizeof quad_tex, 8);
    const uint8_t white[4] = {255, 255, 255, 255};
    gfx_register_texture(0x2002, 1, 1, 28, white, 4, 4);
    const uint8_t bc1_red[8] = {0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};   // c0 = red, c1 = blue, every index 0
    gfx_register_texture(0x2003, 4, 4, 71, bc1_red, 8, 8);
    // Render targets: 0x5000 (RTV 0x5001) and 0x6000 (copy destination), 64x64 RGBA8.
    gfx_register_rendertarget(0x5000, 64, 64, 28, 1);
    gfx_register_rtv(0x5001, 0x5000, 0);
    gfx_register_rendertarget(0x6000, 64, 64, 28, 1);
    gfx_pump_once();   // apply the registrations (shaders compile here)

    // ---- 1: textured quad: maps, the misaligned constant, vertex semantics, orientation -------------------------
    {
        Scene s;
        s.tint.misaligned[1] = 0.5f;   // the green channel is halved: proves the float2 at D3D offset 4 is read there
        s.verts.push_back(quad(-1, -1, 1, 1, 0.5f, 255, 255, 255, 255));
        GfxDraw d = draw_of(s, s.verts[0], kBBRtv, 0x2001);
        gfx_real_draw(&d);
        frame();
        CHECK(px_is(W / 4, H / 4, 255, 0, 0, "top left (red texel)"));
        CHECK(px_is(3 * W / 4, H / 4, 0, 128, 0, "top right (green texel, green halved)"));
        CHECK(px_is(W / 4, 3 * H / 4, 0, 0, 255, "bottom left (blue texel)"));
        CHECK(px_is(3 * W / 4, 3 * H / 4, 255, 128, 255, "bottom right (white texel, green halved)"));
        uint32_t tw = 0, th = 0;
        CHECK(gfx_texture_dims(0x2001, &tw, &th) && tw == 2 && th == 2);
        CHECK(gfx_cbreg_for_mtl(kPS, 0) == 1);   // the shader's first constant buffer is D3D's b1
        printf("scene 1 (textured quad, maps, constant offsets): %s\n", g_failed ? "FAILED" : "ok");
    }
    // ---- 2: depth: the nearer quad wins in either order ----------------------------------------------------------
    for (int order = 0; order < 2; order++) {
        const int before = g_failed;
        Scene s;
        s.verts.push_back(quad(-1, -1, 1, 1, 0.3f, 255, 0, 0, 255));   // near, red
        s.verts.push_back(quad(-1, -1, 1, 1, 0.7f, 0, 0, 255, 255));   // far, blue
        for (int i = 0; i < 2; i++) {
            GfxDraw d = draw_of(s, s.verts[order ? 1 - i : i], kBBRtv, 0x2002);
            d.dsv = 1; d.depth.enable = 1; d.depth.write = 1; d.depth.func = 2;   // LESS
            gfx_real_draw(&d);
        }
        frame();
        CHECK(px_is(W / 2, H / 2, 255, 0, 0, "the nearer quad"));
        printf("scene 2 (depth, %s first): %s\n", order ? "far" : "near", g_failed == before ? "ok" : "FAILED");
    }
    // ---- 3: render to a texture, CopyResource it, sample both ------------------------------------------------------
    {
        const int before = g_failed;
        Scene s;
        s.verts.push_back(quad(-1, -1, 1, 1, 0.5f, 0, 255, 0, 255));       // green, into the render target
        s.verts.push_back(quad(-1, -1, 0, 1, 0.5f, 255, 255, 255, 255));   // left half: the render target
        s.verts.push_back(quad(0, -1, 1, 1, 0.5f, 255, 255, 255, 255));    // right half: its copy
        GfxDraw a = draw_of(s, s.verts[0], 0x5001, 0x2002);
        gfx_real_draw(&a);
        GfxDraw c;
        memset(&c, 0, sizeof c);
        c.is_copy = 1; c.copy_dst = 0x6000; c.copy_src = 0x5000;
        gfx_real_draw(&c);
        GfxDraw l = draw_of(s, s.verts[1], kBBRtv, 0x5000);
        GfxDraw r = draw_of(s, s.verts[2], kBBRtv, 0x6000);
        gfx_real_draw(&l);
        gfx_real_draw(&r);
        frame();
        CHECK(px_is(W / 4, H / 2, 0, 255, 0, "the render target, sampled"));
        CHECK(px_is(3 * W / 4, H / 2, 0, 255, 0, "its CopyResource, sampled"));
        printf("scene 3 (render to texture, copy): %s\n", g_failed == before ? "ok" : "FAILED");
    }
    // ---- 4: alpha blending and scissor -------------------------------------------------------------------------------
    {
        const int before = g_failed;
        Scene s;
        s.verts.push_back(quad(-1, -1, 1, 1, 0.5f, 255, 0, 0, 255));
        s.verts.push_back(quad(-1, -1, 1, 1, 0.5f, 0, 0, 255, 128));
        GfxDraw a = draw_of(s, s.verts[0], kBBRtv, 0x2002);
        gfx_real_draw(&a);
        GfxDraw b = draw_of(s, s.verts[1], kBBRtv, 0x2002);
        b.blend = {1, 5, 6, 1, 2, 6, 1, 0xF};   // SRC_ALPHA, INV_SRC_ALPHA, ADD
        b.rs = 1; b.sc[0] = 0; b.sc[1] = 0; b.sc[2] = (int32_t)W / 2; b.sc[3] = (int32_t)H;
        gfx_real_draw(&b);
        frame();
        CHECK(px_is(W / 4, H / 2, 127, 0, 128, "blended, inside the scissor"));
        CHECK(px_is(3 * W / 4, H / 2, 255, 0, 0, "outside the scissor"));
        printf("scene 4 (blend, scissor): %s\n", g_failed == before ? "ok" : "FAILED");
    }
    // ---- 5: a BC1 texture (native on this GPU, or decoded on the CPU with BO2_VK_NO_BC=1) -------------------------
    {
        const int before = g_failed;
        Scene s;
        s.verts.push_back(quad(-1, -1, 1, 1, 0.5f, 255, 255, 255, 255));
        GfxDraw d = draw_of(s, s.verts[0], kBBRtv, 0x2003);
        gfx_real_draw(&d);
        frame();
        CHECK(px_is(W / 2, H / 2, 255, 0, 0, "the BC1 block"));
        printf("scene 5 (BC1, %s): %s\n", getenv("BO2_VK_NO_BC") ? "decoded on the CPU" : "native if supported",
               g_failed == before ? "ok" : "FAILED");
    }
    // ---- 6: stencil: clear, mark the left half without colour, then draw where the mark is ------------------------
    {
        const int before = g_failed;
        Scene s;
        s.verts.push_back(quad(-1, -1, 0, 1, 0.5f, 0, 255, 0, 255));
        s.verts.push_back(quad(-1, -1, 1, 1, 0.5f, 255, 0, 0, 255));
        GfxDraw clr;
        memset(&clr, 0, sizeof clr);
        clr.is_clear = 2; clr.depth.st_res = 0x7000; clr.depth.st_ref = 0;
        gfx_real_draw(&clr);
        GfxDraw mark = draw_of(s, s.verts[0], kBBRtv, 0x2002);
        mark.dsv = 1;
        mark.blend.write_mask = 0;                          // no colour
        mark.depth.st = 1 | (0xFF << 8) | (0xFF << 16);
        mark.depth.st_ops = (1 | (1 << 4) | (3 << 8) | (8 << 12)) | ((1 | (1 << 4) | (3 << 8) | (8u << 12)) << 16);   // pass: REPLACE, ALWAYS
        mark.depth.st_ref = 1; mark.depth.st_res = 0x7000;
        gfx_real_draw(&mark);
        GfxDraw test = draw_of(s, s.verts[1], kBBRtv, 0x2002);
        test.dsv = 1;
        test.depth.st = 1 | (0xFF << 8) | (0xFF << 16);
        test.depth.st_ops = (1 | (1 << 4) | (1 << 8) | (3 << 12)) | ((1 | (1 << 4) | (1 << 8) | (3u << 12)) << 16);   // EQUAL
        test.depth.st_ref = 1; test.depth.st_res = 0x7000;
        gfx_real_draw(&test);
        frame();
        CHECK(px_is(W / 4, H / 2, 255, 0, 0, "inside the stencil mark"));
        CHECK(px_is(3 * W / 4, H / 2, 13, 13, 18, "outside it (the back buffer's dark grey)"));
        printf("scene 6 (stencil): %s\n", g_failed == before ? "ok" : "FAILED");
    }
    // ---- 7: descriptor sets kept across frames while a texture is released and its id registered again (a new image,
    //      whose view may reuse the destroyed one's handle); draws of one pass sharing their state (the state filter);
    //      an empty scissor rectangle draws nothing ----------------------------------------------------------------
    {
        const int before = g_failed;
        const uint8_t red[4] = {255, 0, 0, 255};
        const uint8_t blue[16] = {0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255};
        gfx_register_texture(0x2004, 1, 1, 28, red, sizeof red, 4);
        auto frame7 = [&](bool check, const char* what, int r, int g, int b) {
            Scene s;
            s.verts.push_back(quad(-1, -1, 0, 1, 0.5f, 255, 255, 255, 255));   // left half: texture 0x2004
            s.verts.push_back(quad(0, -1, 1, 1, 0.5f, 0, 255, 0, 255));        // right half: white texture, green
            s.verts.push_back(quad(-1, -1, 1, 1, 0.5f, 255, 0, 255, 255));     // everywhere, but scissored away
            GfxDraw a = draw_of(s, s.verts[0], kBBRtv, 0x2004);
            GfxDraw c = draw_of(s, s.verts[1], kBBRtv, 0x2002);
            GfxDraw e = draw_of(s, s.verts[2], kBBRtv, 0x2002);
            e.rs = 1;   // scissor on, rectangle (0, 0, 0, 0)
            gfx_real_draw(&a);
            gfx_real_draw(&c);
            gfx_real_draw(&e);
            frame();
            if (!check) return;
            CHECK(px_is(W / 4, H / 2, r, g, b, what));
            CHECK(px_is(3 * W / 4, H / 2, 0, 255, 0, "the pass's second draw (white texture, green)"));
        };
        for (int i = 0; i < 3; i++) frame7(true, "1x1 red, its set kept across frames", 255, 0, 0);
        gfx_release_texture(0x2004);                                      // its image goes once the frames using it are done
        for (int i = 0; i < 4; i++) frame7(i == 3, "released: the declared slot reads transparent black", 0, 0, 0);
        gfx_register_texture(0x2004, 2, 2, 28, blue, sizeof blue, 8);   // the same id again: a new image
        for (int i = 0; i < 4; i++) frame7(true, "registered again as a 2x2 blue", 0, 0, 255);
        printf("scene 7 (descriptor sets kept across frames, texture released and registered again, empty scissor): %s\n",
               g_failed == before ? "ok" : "FAILED");
    }
    // ---- 8: static vertex and index buffers (C50, BO2_IB_RESIDENT): kept on the GPU and drawn in place, from an
    //      offset into the index buffer and from an offset into the vertex buffer -------------------------------------
    {
        const int before = g_failed;
        std::vector<Vtx> vtx = quad(-1, -1, -1.f / 3, 1, 0.5f, 255, 0, 0, 255);       // vertices 0-3: left, red
        for (const Vtx& v : quad(-1.f / 3, -1, 1.f / 3, 1, 0.5f, 0, 255, 0, 255)) vtx.push_back(v);   // 4-7: green
        for (const Vtx& v : quad(1.f / 3, -1, 1, 1, 0.5f, 0, 0, 255, 255)) vtx.push_back(v);          // 8-11: blue
        static const uint16_t idx[18] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7, 0, 1, 2, 2, 1, 3};
        const uint32_t vbytes = (uint32_t)(vtx.size() * sizeof(Vtx));
        for (int f = 0; f < 3; f++) {
            Scene s;
            auto resident = [&](uint32_t first_vtx, uint32_t first_idx) {
                GfxDraw d = draw_of(s, vtx, kBBRtv, 0x2002);
                d.vb = vtx.data() + first_vtx; d.vb_bytes = vbytes - first_vtx * (uint32_t)sizeof(Vtx);   // IASetVertexBuffers offset
                d.res[0] = vtx.data(); d.res_bytes[0] = vbytes;
                d.ib = idx + first_idx; d.ib_bytes = 12;
                d.ib_res = idx; d.ib_res_bytes = sizeof idx;
                gfx_real_draw(&d);
            };
            resident(0, 0);    // red quad: both buffers from their start
            resident(0, 6);    // green quad: indices from the index buffer's 7th (values 4-7)
            resident(8, 12);   // blue quad: vertices from the vertex buffer's 9th, indices 0-3 again
            frame();
            CHECK(px_is(W / 6, H / 2, 255, 0, 0, "resident, from both starts"));
            CHECK(px_is(W / 2, H / 2, 0, 255, 0, "resident, from an index offset"));
            CHECK(px_is(5 * W / 6, H / 2, 0, 0, 255, "resident, from a vertex offset"));
        }
        printf("scene 8 (static vertex and index buffers drawn in place): %s\n", g_failed == before ? "ok" : "FAILED");
    }
    gfx_shutdown();
    if (g_failed) { fprintf(stderr, "vk_backend_test: %d failed\n", g_failed); return 1; }
    printf("vk_backend_test: all passed\n");
    return 0;
}
