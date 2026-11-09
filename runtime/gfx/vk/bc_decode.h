// bc_decode.h — BC1..BC5 to 8-bit RGBA on the CPU (bc_decode.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bo2vk {
// Decodes one level of a DXGI BC1..BC5 texture (formats 70..84) of w x h pixels; `pitch` is the bytes per row of
// blocks (0 = tightly packed). Writes w * h * 4 bytes. *is_signed is set for BC4_SNORM and BC5_SNORM, whose output
// is signed bytes for an R8G8B8A8_SNORM image. False for another format or too few bytes.
bool bc_decode(uint32_t dxgi, const uint8_t* src, size_t src_bytes, uint32_t w, uint32_t h, uint32_t pitch,
               std::vector<uint8_t>& rgba, bool* is_signed = nullptr);
}  // namespace bo2vk
