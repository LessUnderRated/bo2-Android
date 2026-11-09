// Test pixel shader (ours): a dynamically indexed array of matrices in a constant buffer. HLSLcc's GLSL backend
// reports "Unsupported dynamic indexing scheme" for it; the test records that this is detected, not hidden.
cbuffer Skin : register(b1) { float4x3 bones[8]; };
float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0, nointerpolation uint idx : TEXCOORD1) : SV_Target0 {
    return float4(mul(float4(uv, 0, 1), bones[idx & 7]), 1);
}
