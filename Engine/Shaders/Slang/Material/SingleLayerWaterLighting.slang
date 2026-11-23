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
    Texture2D<float4> sceneColor, Texture2D<float> sceneDepth, Texture2D<float> waterCoverage,
    FWaterViewUniform view, float4 viewport, float2 samplePixel,
    float3 viewForward, float surfaceDepth, float slopeTolerance)
{
    float2 texel = ClampWaterPixel(samplePixel, viewport) - 0.5;
    int2 corner = int2(floor(texel));
    float2 fraction = frac(texel);
    float finiteWeight = 0.0;
    float clearWeight = 0.0;
    float3 bedColor = 0.0;
    float3 skyColor = 0.0;
    float depth = 0.0;
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
        if (deviceDepth == view.DepthParameters.x)
        {
            clearWeight += weight;
            skyColor += sceneColor.Load(pixel).rgb * weight;
            continue;
        }
        // Coverage records where water passed the opaque depth test at this tap.
        // On oblique views its water depth can differ greatly from the source pixel's.
        if (waterCoverage.Load(pixel) < 0.5) continue;
        float3 viewPosition;
        if (!ReconstructViewPositionAnalytic(view.Projection[0], view.Projection[1],
                view.Projection[2], view.Projection[3], WaterNdc(float2(pixel.xy) + 0.5, viewport),
                deviceDepth, viewPosition))
            continue;
        float tapDepth = dot(viewPosition, viewForward);
        if (tapDepth + slopeTolerance < surfaceDepth) continue;
        finiteWeight += weight;
        depth += tapDepth * weight;
        bedColor += sceneColor.Load(pixel).rgb * weight;
    }
    FWaterBackground result = (FWaterBackground)0;
    if (finiteWeight > 1.0e-5)
    {
        result.radiance = bedColor / finiteWeight;
        result.depth = depth / finiteWeight;
        result.valid = true;
    }
    else if (clearWeight > 1.0e-5)
    {
        result.radiance = skyColor / clearWeight;
        result.valid = true;
        result.open = true;
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

public float2 WaterRefractionOffsetInPixels(FWaterViewUniform view, float4 viewport, float2 pixel,
    float3 surfaceInView, float3 normal, float3 viewForward, float ior,
    float separation, float depthBiasCm)
{
    // Without a finite receiver there is no hit distance for a screen-space lookup.
    if (ior <= 0.0 || ior == 1.0 || separation <= 0.0) return 0.0;
    bool perspective = dot(view.Projection[3].xyz, view.Projection[3].xyz) > 1.0e-12;
    float3 incident = perspective ? normalize(surfaceInView) : viewForward;
    float3 ray = refract(incident, dot(incident, normal) > 0.0 ? -normal : normal, 1.0 / ior);
    float forward = dot(ray, viewForward);
    if (!all(isfinite(ray)) || forward <= 1.0e-5) return 0.0;

    float4 hit = float4(surfaceInView + ray * (separation / forward), 1.0);
    float w = dot(view.Projection[3], hit);
    if (w <= 1.0e-6) return 0.0;
    float2 ndc = float2(dot(view.Projection[0], hit), dot(view.Projection[1], hit)) / w;
    float2 offset = (ndc - WaterNdc(pixel, viewport)) * (0.5 * viewport.zw);
    float separationCm = separation * 100.0;
    float bias = -depthBiasCm;
    float range = max(1.0, clamp(abs(bias) * 0.5, 0.0, 50.0));
    float fade = saturate(separationCm / range + bias) * saturate(separationCm / 30.0);
    return offset * fade;
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
