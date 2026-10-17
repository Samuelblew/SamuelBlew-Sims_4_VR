// upscale_cas.hlsl: AMD FidelityFX Contrast Adaptive Sharpening (com redimensionamento) como pixel shader.
// Compilado pelo build.bat com o fxc (bytecode embutido na DLL). Licença: MIT (cabeçalho do ffx_cas.h).
#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

Texture2D<float4> InputTexture : register(t0);
cbuffer Consts : register(b0) { uint4 Const0; uint4 Const1; uint4 Const2; uint4 Const3; };

AF3 CasLoad(ASU2 p) { return InputTexture.Load(int3(p, 0)).rgb; }
void CasInput(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_cas.h"

// Saída: o jogo grava cores já em sRGB. Se a imagem do headset só aceita vista _SRGB (o hardware codifica),
// Const3.w = 1 e a cor é linearizada antes (senão os bytes saem iguais aos do jogo).
float3 OutColor(float3 c) {
    if (Const3.w == 0) return c;
    c = saturate(c);
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PSCas(float4 pos : SV_Position) : SV_Target {
    AF1 r, g, b; CasFilter(r, g, b, AU2(pos.xy), Const0, Const1, false);
    return float4(OutColor(float3(r, g, b)), 1);
}
