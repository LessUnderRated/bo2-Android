// Test pixel shader (ours): t3/s1 2D, a cube, a 3D texture, a depth compare, and a Load with an offset.
Texture2D Diffuse : register(t3);
SamplerState DiffuseSampler : register(s1);
TextureCube Reflection : register(t0);
Texture3D Lighting : register(t5);
Texture2D Shadow : register(t7);
SamplerComparisonState ShadowSampler : register(s4);
cbuffer Misc : register(b0) { float Time; float2 StaticScale; float4 Tint; };
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 n : TEXCOORD1; };
float4 main(PSIn i) : SV_Target0 {
    float4 c = Diffuse.Sample(DiffuseSampler, i.uv) * i.col;
    c += Reflection.Sample(DiffuseSampler, i.n) * 0.25;
    c *= Lighting.Sample(DiffuseSampler, float3(i.uv, Time));
    c *= Shadow.SampleCmpLevelZero(ShadowSampler, i.uv, 0.5);
    c += Diffuse.Load(int3(i.pos.xy, 0), int2(1, -1)) * StaticScale.y;
    return c;
}
