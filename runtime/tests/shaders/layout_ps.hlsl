// Test pixel shader (ours): constant-buffer members whose D3D packing std140 does not give by itself, read with
// constant indices. (Dynamic indexing of struct and matrix arrays trips HLSLcc's GLSL backend: see dynindex_ps.hlsl.)
struct Light { float3 dir; float range; float4 color; };
cbuffer Hard : register(b1) {
    float  weights[4];      // D3D: one register per element (stride 16), as std140
    float3 offsets[3];      // stride 16
    float2 pair;            // a new register after the array
    float  after;
    float3 v3;              // D3D: at 124? no - a float3 never crosses a register: next register
    float  w;               // packs after v3 (offset 140)
    float  s0;
    float3 packedv3;        // D3D packs it at s0 + 4 (offset 148): misaligned for std140, split by the translator
    float3x3 rot;           // column_major: 3 registers
    row_major float3x4 rm;  // 3 registers
    Light lights[2];
    float tail;
};
float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    float4 c = weights[2] * float4(offsets[1], after) + float4(packedv3, s0) + float4(v3, w);
    c.xy += pair + mul(rot, float3(uv, 1)).xy;
    c += mul(rm, float4(uv, 1, 1)).xyzz;
    c += lights[1].color * lights[0].range + tail;
    return c;
}
