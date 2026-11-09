// gfx_abi.h — the records bo2-mac's D3D11 layer (remaining_dlls_impl.cpp, the producer) hands its graphics backend.
//
// Copied field for field from the producer, as bo2-mac's metal_backend.mm copies them: the producer has no shared
// header. A divergence would make the backend read every field after it at the wrong address, which produces no
// compiler or link error, only a wrong picture. So the backend compares sizeof(GfxDraw) with the producer's
// gfx_draw_record_size() on the first draw and stops hard on a mismatch (vk_capture.cpp), as bo2-mac does.
#pragma once
#include <cstdint>

namespace bo2vk {

// One element of the bound input layout: the semantic with its index ("TEXCOORD0"), DXGI format, offset, stream.
struct GfxDrawElem { char semantic[24]; uint32_t fmt, offset, slot; };

enum { kMaxPSTex = 16 };       // pixel-shader resource and sampler slots carried per draw
enum { kMaxCB = 8 };           // constant-buffer slots per stage
enum { kMaxVBStreams = 4 };    // vertex streams: slot 0 plus vb2[0..2]

// One D3D11_RENDER_TARGET_BLEND_DESC (render target 0), D3D11 enum values.
struct GfxBlend {
    uint32_t enable;
    uint32_t src_rgb, dst_rgb, op_rgb;
    uint32_t src_a, dst_a, op_a;
    uint32_t write_mask;
};

// D3D11_DEPTH_STENCIL_DESC, D3D11 enum values.
struct GfxDepth {
    uint32_t enable;
    uint32_t write;    // 0 = ZERO, 1 = ALL
    uint32_t func;     // D3D11_COMPARISON_FUNC 1..8
    uint32_t st;       // 0 = StencilEnable FALSE; else 1 | StencilReadMask << 8 | StencilWriteMask << 16
    uint32_t st_ops;   // nibbles, D3D11 values 1..8: front FailOp, DepthFailOp, PassOp, Func (bits 0-15); back (16-31)
    uint32_t st_ref;   // StencilRef of the last OMSetDepthStencilState (8 bits)
    uint32_t st_res;   // resource behind the bound DSV when its format has a stencil plane, else 0
};

struct GfxDraw {
    uint32_t vs, ps;                                 // guest shader object ids
    const GfxDrawElem* elems; uint32_t nelems;       // the input layout
    const void* vb; uint32_t vb_bytes, vb_stride, vb_off;
    const void* vb2[3]; uint32_t vb2_bytes[3], vb2_stride[3];   // streams 1..3
    const void* ib; uint32_t ib_bytes, ib_fmt;       // ib_fmt: DXGI (57 = R16_UINT, else 32-bit)
    uint32_t idx_count, start_idx; int32_t base_vtx;
    uint32_t topo;                                   // D3D11_PRIMITIVE_TOPOLOGY (4 list, 5 strip)
    uint32_t rtv;                                    // render target view (0 = none bound)
    uint32_t is_clear;                               // 1 colour clear of rtv, 2 stencil clear, 3 depth clear
    uint32_t is_copy, copy_dst, copy_src;            // CopyResource between two guest resources
    float clear_rgba[4];
    float vp[6];                                     // viewport x, y, w, h, MinDepth, MaxDepth
    const void* vscb[kMaxCB]; uint32_t vscb_bytes[kMaxCB];
    const void* pscb[kMaxCB]; uint32_t pscb_bytes[kMaxCB];
    uint32_t pstex[kMaxPSTex];                       // resource bound at each t register
    GfxBlend blend;
    GfxDepth depth;
    uint32_t dsv;                                    // depth stencil view (0 = none bound)
    float vpw, vph;
    uint32_t vscb_fill[kMaxCB], pscb_fill[kMaxCB];   // C47: the producer's fill counter per buffer (dedup key)
    // C50: per stream, the start and size of the STATIC buffer vb / vb2 is a slice of (nullptr = copy as usual).
    // Those bytes never change after creation, so they are uploaded once and kept resident.
    const void* res[4]; uint32_t res_bytes[4];
    uint32_t pssamp[kMaxPSTex];                      // sampler bound at each s register (PSSetSamplers)
    uint32_t rs; int32_t sc[4];                      // bit 0 scissor enable (sc = left, top, right, bottom);
                                                     // bit 2 DepthClipEnable = 0; bits 8-9 D3D11_CULL_MODE
                                                     // (2 FRONT, 3 BACK; FrontCounterClockwise already folded in)
    uint32_t dsv_res, dsv_wh;                        // BO2_DEPTH_DSV lever: depth resource and its size (w << 16 | h)
    int32_t zb; float zb_pente, zb_max;              // BO2_DEPTH_BIAS lever: DepthBias, SlopeScaled, Clamp
    uint32_t psmip[kMaxPSTex];                       // SRV mip range per slot: MostDetailedMip << 8 | MipLevels
    // BO2_IB_RESIDENT: the start and size of the STATIC index buffer `ib` is a slice of (nullptr = copy as usual).
    // Set only with resident vertices (res[0]); `ib` then points into it, in place.
    const void* ib_res; uint32_t ib_res_bytes;
};

// The live 2D UI pass (gfx_ui_frame): positions in viewport pixels or through a matrix, an RGBA dword, UVs.
struct UiVert { float x, y, z, u, v; uint8_t r, g, b, a; };
struct UiBatch {
    uint32_t first, count, tex;   // tex = guest texture object id
    uint32_t has_mtx;             // 0 = positions already in viewport pixels
    float mtx[16];                // row-major worldViewProjectionMatrix
};

}  // namespace bo2vk
