// Backend test pixel shader (ours): texture t2 through sampler s3, times the vertex colour, times a constant buffer
// at b1 whose float2 D3D packs at offset 4 (the end-to-end check of dxbc_to_vk.cpp's offset fix).
Texture2D Tex : register(t2);
SamplerState Smp : register(s3);
cbuffer Tint : register(b1) { float scalar; float2 misaligned; float4 tint; };
float4 main(float4 p : SV_Position, float4 col : COLOR0, float2 uv : TEXCOORD0) : SV_Target0 {
    return Tex.Sample(Smp, uv) * col * tint * float4(misaligned, scalar, 1);
}
