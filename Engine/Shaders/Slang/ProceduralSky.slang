// Infinite, linear-radiance analytic sky. No camera position, display mapping,
// scene geometry, fog, clouds, direct sun disc, or environment-light feedback.
public struct FProceduralSkyUniform
{
    public float4 ToSunEnabled;
    public float4 UpHorizonExponent;
    public float4 ZenithExposure;
    public float4 HorizonHaloExponent;
    public float4 Ground;
    public float4 Halo;
    public float4 Tint;
};

public float3 EvaluateProceduralSky(FProceduralSkyUniform sky, float3 direction)
{
    float h = clamp(dot(direction, sky.UpHorizonExponent.xyz), -1.0, 1.0);
    float3 base = h >= 0.0
        ? lerp(sky.HorizonHaloExponent.rgb, sky.ZenithExposure.rgb,
            pow(h, sky.UpHorizonExponent.w))
        : lerp(sky.HorizonHaloExponent.rgb, sky.Ground.rgb, -h);
    float halo = pow(saturate(dot(direction, sky.ToSunEnabled.xyz)),
        sky.HorizonHaloExponent.w) * smoothstep(-0.05, 0.05, h);
    return clamp((base + sky.Halo.rgb * halo) * sky.Tint.rgb
        * exp2(sky.ZenithExposure.w), 0.0, 16384.0);
}

public float3 SkyCubeDirection(uint face, float2 uv)
{
    float2 p = uv * 2.0 - 1.0;
    switch (face)
    {
    case 0: return normalize(float3( 1.0, -p.y, -p.x));
    case 1: return normalize(float3(-1.0, -p.y,  p.x));
    case 2: return normalize(float3( p.x,  1.0,  p.y));
    case 3: return normalize(float3( p.x, -1.0, -p.y));
    case 4: return normalize(float3( p.x, -p.y,  1.0));
    default:return normalize(float3(-p.x, -p.y, -1.0));
    }
}
