// SDR, opaque window capture. AMD reference headers are vendored unmodified.
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"
#if defined(EASU_PASS)
#define FSR_EASU_F 1
#elif defined(RCAS_PASS)
#define FSR_RCAS_F 1
#endif
#include "ffx_fsr1.h"

Texture2D<float4> image : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Params : register(b0) {
    float2 inputSize;
    float2 outputSize;
    float strength; // App scale: 0 = bypass; 1 = maximum. NOT RCAS stops.
    float3 padding;
};
struct V { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
V VSMain(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
#if defined(EASU_PASS)
AF4 FsrEasuRF(AF2 p) { return image.GatherRed(linearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return image.GatherGreen(linearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return image.GatherBlue(linearClamp, p); }
float4 PSMain(V v) : SV_TARGET {
    uint4 c0, c1, c2, c3;
    FsrEasuCon(c0, c1, c2, c3, inputSize.x, inputSize.y,
        inputSize.x, inputSize.y, outputSize.x, outputSize.y);
    float3 color;
    FsrEasuF(color, uint2(v.position.xy), c0, c1, c2, c3);
    return float4(saturate(color), 1);
}
#elif defined(RCAS_PASS)
AF4 FsrRcasLoadF(ASU2 p) {
    return image.Load(int3(clamp(p, int2(0, 0), int2(outputSize) - 1), 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) { }
float4 PSMain(V v) : SV_TARGET {
    if (strength <= 0) return FsrRcasLoadF(int2(v.position.xy));
    uint4 c;
    FsrRcasCon(c, -log2(clamp(strength, 0.0001, 1.0)));
    float3 color;
    FsrRcasF(color.r, color.g, color.b, uint2(v.position.xy), c);
    return float4(saturate(color), 1);
}
#else
float4 PSMain(V v) : SV_TARGET { return float4(image.SampleLevel(linearClamp, v.uv, 0).rgb, 1); }
#endif