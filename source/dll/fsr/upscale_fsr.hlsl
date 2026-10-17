// upscale_fsr.hlsl: AMD FidelityFX Super Resolution 1.0 (EASU + RCAS) como pixel shaders.
// Compilado pelo build.bat com o fxc (bytecode embutido na DLL). Licença do FSR: fsr\license.txt (MIT).
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

Texture2D<float4> InputTexture : register(t0);
SamplerState      LinearClamp  : register(s0);
cbuffer Consts : register(b0) { uint4 Const0; uint4 Const1; uint4 Const2; uint4 Const3; };

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(LinearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(LinearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(LinearClamp, p); }

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

// Saída: o jogo grava cores já em sRGB. Se a imagem do headset só aceita vista _SRGB (o hardware codifica),
// Const3.w = 1 e a cor é linearizada antes (senão os bytes saem iguais aos do jogo).
float3 OutColor(float3 c) {
    if (Const3.w == 0) return c;
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

// Triângulo de tela cheia sem buffer de vértices.
float4 VSFull(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSEasu(float4 pos : SV_Position) : SV_Target {
    AF3 c; FsrEasuF(c, AU2(pos.xy), Const0, Const1, Const2, Const3);
    return float4(OutColor(c), 1);
}

float4 PSRcas(float4 pos : SV_Position) : SV_Target {
    AF1 r, g, b; FsrRcasF(r, g, b, AU2(pos.xy), Const0);
    return float4(OutColor(float3(r, g, b)), 1);
}

// Bilinear simples (Const0.xy = 1 / tamanho do retângulo de saída, Const0.zw = canto do retângulo).
float4 PSBilinear(float4 pos : SV_Position) : SV_Target {
    return float4(OutColor(InputTexture.SampleLevel(LinearClamp, (pos.xy - asfloat(Const0.zw)) * asfloat(Const0.xy), 0).rgb), 1);
}
