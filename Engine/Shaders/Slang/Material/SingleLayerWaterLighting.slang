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
        if (waterCoverage.Load(pixel) < 0.5) continue;
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

// Rotate the incident ray by the angular change between refraction at the
// mesh normal and at the shading normal. This preserves Pixel Normal Offset's
// flat-normal identity while using Snell refraction for the wave perturbation.
public float3 WaterPixelNormalRay(float3 incident, float3 reference, float3 transmitted)
{
    float3 axis = cross(reference, transmitted);
    return normalize(incident + cross(axis, incident)
        + cross(axis, cross(axis, incident)) / max(1.0 + dot(reference, transmitted), 1.0e-6));
}

float WaterDepthSlope(float before, float center, float after)
{
    float a = center - before, b = after - center;
    return a * b > 0.0 ? sign(a) * min(abs(a), abs(b)) : 0.0;
}

float3 WaterHitRadiance(Texture2D<float4> color, Texture2D<float> depth,
    Texture2D<float> coverage, float clearDepth, float2 pixel, int2 cell, int2 lo, int2 hi,
    float center, float2 gradient)
{
    // Filter color on the hit surface. Interpolating across a
    // depth edge would reintroduce a stripe from the other face.
    float2 texel = pixel - 0.5;
    int2 corner = int2(floor(texel));
    float2 fraction = frac(texel);
    float sum = 0.0;
    float3 radiance = 0.0;
    [unroll]
    for (uint tap = 0; tap < 4; ++tap)
    {
        int2 shift = int2(tap & 1u, tap >> 1u);
        int2 pos = clamp(corner + shift, lo, hi);
        float sampleZ = depth.Load(int3(pos, 0));
        float expected = center + dot(gradient, float2(pos - cell));
        if (sampleZ == clearDepth || coverage.Load(int3(pos, 0)) < 0.5
            || abs(sampleZ - expected) > max(2.0e-7, dot(abs(gradient), float2(0.5)))) continue;
        float2 weights = lerp(1.0 - fraction, fraction, float2(shift));
        float weight = weights.x * weights.y;
        radiance += color.Load(int3(pos, 0)).rgb * weight;
        sum += weight;
    }
    return sum > 0.0 ? radiance / sum : color.Load(int3(cell, 0)).rgb;
}

// A projected ray is linear in pixel XY and device Z with parameter
// q = distance / clipW(distance). Test only the interval inside each texel:
// stepping behind a nearer silhouette is not an intersection with its face.
public bool TraceWaterReceiver(Texture2D<float4> color, Texture2D<float> depth,
    Texture2D<float> coverage, FWaterViewUniform view, float4 viewport, float2 pixel,
    float3 origin, float3 direction, float3 forward, out FWaterBackground hit, out float distance,
    out float2 hitPixel)
{
    hit = (FWaterBackground)0;
    distance = 0.0;
    hitPixel = pixel;
    float4 point = float4(origin, 1.0);
    float clipW = dot(view.Projection[3], point);
    if (clipW <= 0.0) return false;
    float dw = dot(view.Projection[3].xyz, direction);
    float z = dot(view.Projection[2], point) / clipW;
    float dz = dot(view.Projection[2].xyz, direction) - z * dw;
    float2 xy = float2(dot(view.Projection[0].xyz, direction), dot(view.Projection[1].xyz, direction));
    float2 velocity = (xy - WaterNdc(pixel, viewport) * dw) * viewport.zw * 0.5;
    float end = dw > 0.0 ? 1.0 / dw : 1.0e20;
    if (dz != 0.0) end = min(end, ((dz > 0.0 ? 1.0 : 0.0) - z) / dz);
    int2 cell = int2(floor(pixel)), step = int2(sign(velocity));
    int2 lo = int2(viewport.xy), hi = lo + int2(viewport.zw) - 1;
    float2 stride = 1.0 / max(abs(velocity), 1.0e-20);
    float2 next = abs(float2(cell) + float2(step.x > 0, step.y > 0) - pixel) * stride;
    float enter = 0.0;
    [loop]
    for (uint visit = 0; visit < 192 && enter < end; ++visit)
    {
        if (any(cell < lo) || any(cell > hi)) break;
        float leave = min(end, min(next.x, next.y));
        float center = depth.Load(int3(cell, 0));
        if (center != view.DepthParameters.x && coverage.Load(int3(cell, 0)) >= 0.5)
        {
            float2 gradient = float2(
                WaterDepthSlope(depth.Load(int3(max(cell - int2(1, 0), lo), 0)), center,
                    depth.Load(int3(min(cell + int2(1, 0), hi), 0))),
                WaterDepthSlope(depth.Load(int3(max(cell - int2(0, 1), lo), 0)), center,
                    depth.Load(int3(min(cell + int2(0, 1), hi), 0))));
            float denominator = dz - dot(gradient, velocity);
            float receiverZ = center + dot(gradient, pixel - (float2(cell) + 0.5));
            if (denominator != 0.0)
            {
                float q = (receiverZ - z) / denominator;
                if (q >= enter && q <= leave && 1.0 - q * dw > 0.0)
                {
                    distance = q * clipW / (1.0 - q * dw);
                    hitPixel = pixel + velocity * q;
                    hit.depth = dot(origin + distance * direction, forward);
                    hit.valid = true;
                    hit.radiance = WaterHitRadiance(color, depth, coverage, view.DepthParameters.x,
                        hitPixel, cell, lo, hi, center, gradient);
                    return true;
                }
            }
        }
        bool advanceX = next.x <= next.y;
        bool advanceY = next.y <= next.x;
        if (advanceX) { cell.x += step.x; next.x += stride.x; }
        if (advanceY) { cell.y += step.y; next.y += stride.y; }
        enter = leave;
    }
    return false;
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
