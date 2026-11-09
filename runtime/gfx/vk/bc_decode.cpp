// bc_decode.cpp — BC1..BC5 to 8-bit RGBA on the CPU, for GPUs without textureCompressionBC.
//
// BC1, BC2, BC3 and BC5 are bo2-mac's decoder (metal_backend.mm, used there to give BC textures a mip chain). Added
// here: BC4, and the SNORM forms of BC4 and BC5 (decoded to signed bytes, uploaded as R8G8B8A8_SNORM). Missing
// channels follow D3D's sampling rules: BC4 returns (r, 0, 0, 1), BC5 (r, g, 0, 1).
#include "bc_decode.h"

#include <cstring>

namespace bo2vk {
namespace {

void color_block(const uint8_t* b, uint8_t out[16][4], bool bc1_punch) {
    const uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8));
    const uint16_t c1 = (uint16_t)(b[2] | (b[3] << 8));
    uint8_t r[4], g[4], bl[4];
    auto unpack = [](uint16_t c, uint8_t& R, uint8_t& G, uint8_t& B) {
        R = (uint8_t)(((c >> 11) & 0x1f) * 255 / 31);
        G = (uint8_t)(((c >> 5) & 0x3f) * 255 / 63);
        B = (uint8_t)((c & 0x1f) * 255 / 31);
    };
    unpack(c0, r[0], g[0], bl[0]);
    unpack(c1, r[1], g[1], bl[1]);
    if (!bc1_punch || c0 > c1) {
        r[2] = (uint8_t)((2 * r[0] + r[1]) / 3); g[2] = (uint8_t)((2 * g[0] + g[1]) / 3); bl[2] = (uint8_t)((2 * bl[0] + bl[1]) / 3);
        r[3] = (uint8_t)((r[0] + 2 * r[1]) / 3); g[3] = (uint8_t)((g[0] + 2 * g[1]) / 3); bl[3] = (uint8_t)((bl[0] + 2 * bl[1]) / 3);
    } else {
        r[2] = (uint8_t)((r[0] + r[1]) / 2); g[2] = (uint8_t)((g[0] + g[1]) / 2); bl[2] = (uint8_t)((bl[0] + bl[1]) / 2);
        r[3] = g[3] = bl[3] = 0;
    }
    const uint32_t idx = (uint32_t)(b[4] | (b[5] << 8) | (b[6] << 16) | ((uint32_t)b[7] << 24));
    for (int i = 0; i < 16; i++) {
        const int k = (idx >> (i * 2)) & 3;
        out[i][0] = r[k]; out[i][1] = g[k]; out[i][2] = bl[k];
        out[i][3] = (bc1_punch && c0 <= c1 && k == 3) ? 0 : 255;
    }
}

// The BC3 alpha / BC4 channel block, unsigned.
void alpha_block(const uint8_t* b, uint8_t out[16]) {
    uint8_t a[8];
    a[0] = b[0]; a[1] = b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t bits = 0;
    for (int i = 2; i < 8; i++) bits |= (uint64_t)b[i] << ((i - 2) * 8);
    for (int i = 0; i < 16; i++) out[i] = a[(bits >> (i * 3)) & 7];
}

// The same block, signed (BC4_SNORM, BC5_SNORM): endpoints are int8, -128 reads as -127; the 4-value mode ends with
// -1.0 and 1.0.
void alpha_block_signed(const uint8_t* b, int8_t out[16]) {
    int a[8];
    a[0] = (int8_t)b[0] < -127 ? -127 : (int8_t)b[0];
    a[1] = (int8_t)b[1] < -127 ? -127 : (int8_t)b[1];
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
    else { for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5; a[6] = -127; a[7] = 127; }
    uint64_t bits = 0;
    for (int i = 2; i < 8; i++) bits |= (uint64_t)b[i] << ((i - 2) * 8);
    for (int i = 0; i < 16; i++) out[i] = (int8_t)a[(bits >> (i * 3)) & 7];
}

}  // namespace

bool bc_decode(uint32_t dxgi, const uint8_t* src, size_t src_bytes, uint32_t w, uint32_t h, uint32_t pitch,
               std::vector<uint8_t>& rgba, bool* is_signed) {
    const bool bc1 = dxgi >= 70 && dxgi <= 72, bc2 = dxgi >= 73 && dxgi <= 75, bc3 = dxgi >= 76 && dxgi <= 78;
    const bool bc4 = dxgi >= 79 && dxgi <= 81, bc5 = dxgi >= 82 && dxgi <= 84;
    const bool snorm = dxgi == 81 || dxgi == 84;
    if (!bc1 && !bc2 && !bc3 && !bc4 && !bc5) return false;
    if (is_signed) *is_signed = snorm;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const uint32_t block = (bc1 || bc4) ? 8u : 16u;
    const size_t row = pitch ? pitch : (size_t)bw * block;
    if (row < (size_t)bw * block || (uint64_t)row * (bh - 1) + (uint64_t)bw * block > src_bytes) return false;
    rgba.assign((size_t)w * h * 4, 0);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* blk = src + by * row + (size_t)bx * block;
            uint8_t px[16][4] = {};
            if (bc1) {
                color_block(blk, px, true);
            } else if (bc2) {
                color_block(blk + 8, px, false);
                for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)(((blk[i / 2] >> ((i & 1) ? 4 : 0)) & 0xf) * 17);
            } else if (bc3) {
                uint8_t al[16];
                color_block(blk + 8, px, false);
                alpha_block(blk, al);
                for (int i = 0; i < 16; i++) px[i][3] = al[i];
            } else if (!snorm) {
                uint8_t c0[16], c1[16] = {};
                alpha_block(blk, c0);
                if (bc5) alpha_block(blk + 8, c1);
                for (int i = 0; i < 16; i++) { px[i][0] = c0[i]; px[i][1] = bc5 ? c1[i] : 0; px[i][2] = 0; px[i][3] = 255; }
            } else {
                int8_t c0[16], c1[16] = {};
                alpha_block_signed(blk, c0);
                if (bc5) alpha_block_signed(blk + 8, c1);
                for (int i = 0; i < 16; i++) {
                    px[i][0] = (uint8_t)c0[i]; px[i][1] = (uint8_t)(bc5 ? c1[i] : 0); px[i][2] = 0; px[i][3] = 127;   // 1.0
                }
            }
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                memcpy(rgba.data() + ((size_t)y * w + x) * 4, px[i], 4);
            }
        }
    return true;
}

}  // namespace bo2vk
