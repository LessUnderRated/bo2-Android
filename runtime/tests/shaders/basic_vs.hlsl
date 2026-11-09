// Test vertex shader (ours): b0 with a float then a float2 (D3D puts it at 20), b2 not contiguous with b0.
cbuffer Misc : register(b0) { float Time; float2 StaticScale; float4 Tint; };
cbuffer Object : register(b2) { float4x4 WorldViewProj; float3 EyePos; float Fade; };
struct VSIn { float4 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD0; float3 nrm : NORMAL; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 n : TEXCOORD1; };
VSOut main(VSIn i) {
    VSOut o;
    o.pos = mul(i.pos, WorldViewProj);
    o.uv = i.uv * StaticScale + Time;
    o.col = i.col * Tint * Fade;
    o.n = normalize(i.nrm - EyePos);
    return o;
}
