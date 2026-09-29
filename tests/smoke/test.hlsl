#ifndef USE_TINT
#define USE_TINT 0
#endif

float4 main(float4 position : SV_Position) : SV_Target
{
#if USE_TINT
    return float4(0.25, 0.5, 0.75, 1.0);
#else
    return float4(1.0, 1.0, 1.0, 1.0);
#endif
}
