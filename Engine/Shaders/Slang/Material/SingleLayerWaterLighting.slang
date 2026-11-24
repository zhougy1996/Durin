module SingleLayerWaterLighting;

import Material.GBufferDecode;

public struct FWaterViewUniform
{
    public float4 Projection[4];
    public float4 DepthParameters;
};

public struct FWaterBackground
{
    public float3 radiance;
    public float depth;
    public bool valid;
    public bool open;
};

public float3 WaterViewForward(FWaterViewUniform view)
{
    float3 perspectiveAxis = view.Projection[3].xyz;
    float3 orthographicAxis = view.Projection[2].xyz * (view.DepthParameters.x == 0.0 ? -1.0 : 1.0);
    return normalize(dot(perspectiveAxis, perspectiveAxis) > 1.0e-12 ? perspectiveAxis : orthographicAxis);
}

float2 WaterNdc(float2 pixel, float4 viewport)
{
    return (pixel - viewport.xy) / viewport.zw * 2.0 - 1.0;
}

public float2 ClampWaterPixel(float2 pixel, float4 viewport)
{
    float2 middle = viewport.xy + 0.5 * viewport.zw;
    return clamp(pixel, min(viewport.xy + 0.55, middle),
        max(viewport.xy + viewport.zw - 0.55, middle));
}

public FWaterBackground SampleWaterBackground(
    Texture2D<float4> sceneColor, Texture2D<float> sceneDepth,
    FWaterViewUniform view, float4 viewport, float2 samplePixel,
    float3 viewForward, float surfaceDepth, float slopeTolerance)
{
    float2 texel = ClampWaterPixel(samplePixel, viewport) - 0.5;
    int2 corner = int2(floor(texel));
    float2 fraction = frac(texel);
    float totalWeight = 0.0;
    float3 radiance = 0.0;
    float depth = 0.0;
    bool allClear = true;
    [unroll]
    for (uint tap = 0; tap < 4; ++tap)
    {
        int2 offset = int2(tap & 1u, tap >> 1u);
        float2 bilinear = lerp(1.0 - fraction, fraction, float2(offset));
        float weight = bilinear.x * bilinear.y;
        if (weight <= 0.0) continue;
        int3 pixel = int3(corner + offset, 0);
        float deviceDepth = sceneDepth.Load(pixel);
        if (!isfinite(deviceDepth)) continue;
        bool clear = deviceDepth == view.DepthParameters.x;
        float3 viewPosition;
        float tapDepth;
        if (ReconstructViewPositionAnalytic(view.Projection[0], view.Projection[1],
                view.Projection[2], view.Projection[3], WaterNdc(float2(pixel.xy) + 0.5, viewport),
                deviceDepth, viewPosition))
            tapDepth = dot(viewPosition, viewForward);
        else if (clear)
            tapDepth = 1.0e10; // Finite representation of the infinite far plane for filtering.
        else continue;
        if (!clear && tapDepth + slopeTolerance < surfaceDepth) continue;
        allClear = allClear && clear;
        totalWeight += weight;
        depth += tapDepth * weight;
        radiance += sceneColor.Load(pixel).rgb * weight;
    }
    FWaterBackground result = (FWaterBackground)0;
    if (totalWeight > 1.0e-5)
    {
        result.radiance = radiance / totalWeight;
        result.depth = depth / totalWeight;
        result.valid = true;
        result.open = allClear;
    }
    return result;
}

// Recover the position on the original view ray at the selected linear depth.
public float3 ReconstructWaterBed(FWaterViewUniform view, float4 viewport,
    float2 pixel, float3 viewForward, float depth)
{
    float2 ndc = WaterNdc(pixel, viewport);
    float4 rowX = view.Projection[0] - view.Projection[3] * ndc.x;
    float4 rowY = view.Projection[1] - view.Projection[3] * ndc.y;
    float3 ray = cross(rowX.xyz, rowY.xyz);
    float determinant = dot(viewForward, ray);
    if (abs(determinant) <= 1.0e-20) return viewForward * depth;
    return (depth * ray - rowX.w * cross(rowY.xyz, viewForward)
        - rowY.w * cross(viewForward, rowX.xyz)) / determinant;
}

public float WaterIor(float specular)
{
    float f0 = 0.08 * specular;
    return 2.0 / (1.0 - sqrt(min(f0, 0.99))) - 1.0;
}

public float3 WaterIncidentDirection(FWaterViewUniform view, float3 viewPosition, float3 forward)
{
    return dot(view.Projection[3].xyz, view.Projection[3].xyz) > 1.0e-12 ? normalize(viewPosition) : forward;
}

public float2 WaterDistortionPixels(FWaterViewUniform view, float2 viewSize, float3 viewNormal,
    float3 forward, float strength, float separationCm, float depthBiasCm, bool open)
{
    // Projection rows give screen right and down in Durin view space.
    float3 right = view.Projection[0].xyz - forward * dot(view.Projection[0].xyz, forward);
    float3 down = view.Projection[1].xyz - forward * dot(view.Projection[1].xyz, forward);
    float2 screenNormal = float2(dot(viewNormal, normalize(right)), dot(viewNormal, normalize(down)));
    float range = clamp(abs(depthBiasCm) * 0.5, 1.0, 50.0);
    float fade = open ? 1.0 : saturate(separationCm / range - depthBiasCm) * saturate(separationCm / 30.0);
    // UE's mono FOV/aspect correction leaves the unscaled view width on both axes.
    // Durin renders the scene snapshot at the active viewport's native resolution.
    return screenNormal * viewSize * (viewSize.x * length(right) * (strength - 1.0) * 0.00023 * 4.0 * fade);
}

public float WaterSchlickPhase(float g, float rayLightCosine)
{
    // The cubic overshoots unity near either endpoint. Bound the directional peak as well.
    float k = clamp(1.55 * g - 0.55 * g * g * g, -0.99, 0.99);
    float denominator = max(abs(1.0 + k * rayLightCosine), 1.0e-4);
    return (1.0 - k * k) / (12.5663706144 * denominator * denominator);
}

public float3 WaterTransmission(float3 extinction, float distanceCm, bool open)
{
    if (open) return select(extinction > 0.0, float3(0.0), float3(1.0));
    return exp(-extinction * distanceCm);
}

public float3 WaterIntegratedScattering(float3 scattering, float3 extinction, float3 transmission)
{
    return saturate(scattering * (1.0 - transmission) / max(extinction, 1.0e-5));
}
