// Backend test vertex shader (ours): position scaled and offset by a constant buffer, colour and UV passed through.
cbuffer Xform : register(b0) { float4 offset_scale; };   // xy offset, zw scale
struct VSIn { float3 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };
VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos.xy * offset_scale.zw + offset_scale.xy, i.pos.z, 1);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}
