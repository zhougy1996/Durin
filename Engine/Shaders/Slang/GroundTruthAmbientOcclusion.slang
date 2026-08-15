module GroundTruthAmbientOcclusion;

import Material.GBufferDecode;

static const float GTAO_PI = 3.14159265358979323846;
static const float GTAO_HALF_PI = 1.57079632679489661923;

float FastAcos(float value)
{
    float x = abs(clamp(value, -1.0, 1.0));
    float result = (-0.156583 * x + GTAO_HALF_PI)
        * sqrt(max(0.0, 1.0 - x));
    return value >= 0.0 ? result : GTAO_PI - result;
}

struct VSInput
{
    float2 pos : POSITION;
    float2 uv : TEXCOORD;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
};

struct FGTAOViewUniform
{
    float4 ProjectionRow0;
    float4 ProjectionRow1;
    float4 ProjectionRow2;
    float4 ProjectionRow3;
    float4x4 WorldToView;
    float4 Viewport; // xy origin, zw extent
    float4 Controls; // radius, falloff start fraction, max pixels, thickness
};

struct FGTAOFilterUniform
{
    float4 ProjectionRow0;
    float4 ProjectionRow1;
    float4 ProjectionRow2;
    float4 ProjectionRow3;
    float4 Viewport;
    float4 DirectionAndThresholds; // xy tap direction, z normal dot, w relative depth
};

[[vk::binding(0, 0)]] Texture2D<float4> GBufferNormals;
[[vk::binding(1, 0)]] Texture2D<float4> GBufferSurface;
[[vk::binding(2, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(3, 0)]] ConstantBuffer<FGTAOViewUniform> View;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

bool ReconstructAt(int2 pixel, out float3 viewPosition)
{
    float2 local = float2(pixel) + 0.5 - View.Viewport.xy;
    float2 uv = local / View.Viewport.zw;
    float2 ndc = uv * 2.0 - 1.0;
    return ReconstructViewPositionAnalytic(
        View.ProjectionRow0, View.ProjectionRow1,
        View.ProjectionRow2, View.ProjectionRow3,
        ndc, SceneDepth.Load(int3(pixel, 0)), viewPosition);
}

float2 FixedNoise(int2 pixel)
{
    uint index = (uint(pixel.x) & 3u) | ((uint(pixel.y) & 3u) << 2u);
    uint permuted = (index * 9u + 5u) & 15u;
    return float2(
        (float(permuted) + 0.5) / 16.0,
        (float((permuted * 7u + 3u) & 15u) + 0.5) / 16.0);
}

[shader("fragment")]
float RawFragmentMain(VSOutput input) : SV_Target0
{
    int2 pixel = int2(input.pos.xy);
    float4 surface = GBufferSurface.Load(int3(pixel, 0));
    uint flags = (uint)round(saturate(surface.a) * 255.0);
    if ((flags & GBufferStandardLitFlag) == 0u)
        return 1.0;

    float3 centerPosition;
    if (!ReconstructAt(pixel, centerPosition))
        return 1.0;
    float3 worldNormal = DecodeOctahedralNormal(
        GBufferNormals.Load(int3(pixel, 0)).ba);
    float3 viewNormal = mul((float3x3)View.WorldToView, worldNormal);
    float normalLengthSquared = dot(viewNormal, viewNormal);
    if (!all(isfinite(viewNormal)) || normalLengthSquared <= 1.0e-8)
        return 1.0;
    viewNormal *= rsqrt(normalLengthSquared);
    float3 viewVector = normalize(-centerPosition);

    float projectedScale = length(View.ProjectionRow1.xyz)
        * View.Viewport.w * 0.5;
    float screenRadius = min(View.Controls.z,
        View.Controls.x * projectedScale / max(abs(centerPosition.z), 1.0e-4));
    if (!isfinite(screenRadius) || screenRadius < 1.3)
        return 1.0;

    float2 noise = FixedNoise(pixel);
    float visibility = 0.0;
    [unroll]
    for (uint slice = 0u; slice < 3u; ++slice)
    {
        float phi = (float(slice) + noise.x) * GTAO_PI / 3.0;
        float cosPhi = cos(phi);
        float sinPhi = sin(phi);
        float2 omega = float2(cosPhi, -sinPhi) * screenRadius;
        // Durin view space is X-forward; screen X/Y map to +Y/-Z.
        float3 direction = float3(0.0, cosPhi, -sinPhi);
        float3 orthoDirection = direction
            - dot(direction, viewVector) * viewVector;
        float3 axis = normalize(cross(orthoDirection, viewVector));
        float3 projectedNormal = viewNormal
            - axis * dot(viewNormal, axis);
        float projectedNormalLength = length(projectedNormal);
        if (projectedNormalLength <= 1.0e-5)
        {
            visibility += 1.0;
            continue;
        }
        float signNormal = dot(orthoDirection, projectedNormal) >= 0.0
            ? 1.0 : -1.0;
        float cosNormal = saturate(
            dot(projectedNormal, viewVector) / projectedNormalLength);
        float normalAngle = signNormal * FastAcos(cosNormal);
        float lowHorizon0 = cos(normalAngle + GTAO_HALF_PI);
        float lowHorizon1 = cos(normalAngle - GTAO_HALF_PI);
        float lowH0 = -FastAcos(lowHorizon1);
        float lowH1 = FastAcos(lowHorizon0);
        float lowArc0 = (cosNormal + 2.0 * lowH0 * sin(normalAngle)
            - cos(2.0 * lowH0 - normalAngle)) * 0.25;
        float lowArc1 = (cosNormal + 2.0 * lowH1 * sin(normalAngle)
            - cos(2.0 * lowH1 - normalAngle)) * 0.25;
        float unoccludedSlice = projectedNormalLength * (lowArc0 + lowArc1);
        float horizon0 = lowHorizon0;
        float horizon1 = lowHorizon1;
        [unroll]
        for (uint step = 0u; step < 4u; ++step)
        {
            float radial = (float(step) + 0.5 + noise.y) / 4.0;
            radial = min(radial, 1.0);
            int2 offset = int2(round(omega * radial));
            if (all(offset == int2(0, 0)))
                offset = int2(cosPhi >= 0.0 ? 1 : -1,
                    sinPhi >= 0.0 ? -1 : 1);
            [unroll]
            for (uint side = 0u; side < 2u; ++side)
            {
                int2 samplePixel = pixel + (side == 0u ? offset : -offset);
                int2 viewportMin = int2(View.Viewport.xy);
                int2 viewportMax = viewportMin + int2(View.Viewport.zw);
                if (any(samplePixel < viewportMin)
                    || any(samplePixel >= viewportMax))
                    continue;
                float3 samplePosition;
                if (!ReconstructAt(samplePixel, samplePosition))
                    continue;
                float3 delta = samplePosition - centerPosition;
                float distance = length(delta);
                if (!isfinite(distance) || distance <= 1.0e-5
                    || distance > View.Controls.x + View.Controls.w)
                    continue;
                // Coplanar reconstruction error is not an occluder. A sample
                // must protrude toward the receiver hemisphere by the frozen
                // thickness before it may raise either horizon.
                if (dot(delta, viewNormal) <= View.Controls.w)
                    continue;
                float falloffStart = View.Controls.x * View.Controls.y;
                float weight = 1.0 - smoothstep(
                    falloffStart, View.Controls.x, max(0.0, distance - View.Controls.w));
                float horizon = dot(delta / distance, viewVector);
                if (side == 0u)
                    horizon0 = max(horizon0, lerp(lowHorizon0, horizon, weight));
                else
                    horizon1 = max(horizon1, lerp(lowHorizon1, horizon, weight));
            }
        }
        float h0 = -FastAcos(horizon1);
        float h1 = FastAcos(horizon0);
        float arc0 = (cosNormal + 2.0 * h0 * sin(normalAngle)
            - cos(2.0 * h0 - normalAngle)) * 0.25;
        float arc1 = (cosNormal + 2.0 * h1 * sin(normalAngle)
            - cos(2.0 * h1 - normalAngle)) * 0.25;
        float occludedSlice = projectedNormalLength * (arc0 + arc1);
        visibility += unoccludedSlice > 1.0e-5
            ? saturate(occludedSlice / unoccludedSlice) : 1.0;
    }
    visibility = saturate(visibility / 3.0);
    return all(isfinite(visibility)) ? visibility : 1.0;
}

[[vk::binding(4, 0)]] Texture2D<float> FilterInputVisibility;
[[vk::binding(5, 0)]] Texture2D<float4> FilterGBufferNormals;
[[vk::binding(6, 0)]] Texture2D<float4> FilterGBufferSurface;
[[vk::binding(7, 0)]] Texture2D<float> FilterSceneDepth;
[[vk::binding(8, 0)]] ConstantBuffer<FGTAOFilterUniform> Filter;

bool ReconstructFilterAt(int2 pixel, out float3 viewPosition)
{
    float2 local = float2(pixel) + 0.5 - Filter.Viewport.xy;
    float2 ndc = local / Filter.Viewport.zw * 2.0 - 1.0;
    return ReconstructViewPositionAnalytic(
        Filter.ProjectionRow0, Filter.ProjectionRow1,
        Filter.ProjectionRow2, Filter.ProjectionRow3,
        ndc, FilterSceneDepth.Load(int3(pixel, 0)), viewPosition);
}

[shader("fragment")]
float BilateralFragmentMain(VSOutput input) : SV_Target0
{
    int2 pixel = int2(input.pos.xy);
    float4 centerSurface = FilterGBufferSurface.Load(int3(pixel, 0));
    uint centerFlags = (uint)round(saturate(centerSurface.a) * 255.0);
    if ((centerFlags & GBufferStandardLitFlag) == 0u)
        return 1.0;

    float3 centerPosition;
    if (!ReconstructFilterAt(pixel, centerPosition))
        return 1.0;
    float3 centerNormal = DecodeOctahedralNormal(
        FilterGBufferNormals.Load(int3(pixel, 0)).ba);
    if (!all(isfinite(centerNormal)))
        return 1.0;

    static const float weights[5] = {
        0.06136, 0.24477, 0.38774, 0.24477, 0.06136};
    float weightedVisibility = 0.0;
    float totalWeight = 0.0;
    int2 viewportMin = int2(Filter.Viewport.xy);
    int2 viewportMax = viewportMin + int2(Filter.Viewport.zw);
    [unroll]
    for (int tap = -2; tap <= 2; ++tap)
    {
        int2 samplePixel = pixel
            + int2(Filter.DirectionAndThresholds.xy) * tap;
        if (any(samplePixel < viewportMin) || any(samplePixel >= viewportMax))
            continue;
        float weight = weights[tap + 2];
        if (tap != 0)
        {
            float4 sampleSurface =
                FilterGBufferSurface.Load(int3(samplePixel, 0));
            uint sampleFlags =
                (uint)round(saturate(sampleSurface.a) * 255.0);
            float3 samplePosition;
            float3 sampleNormal = DecodeOctahedralNormal(
                FilterGBufferNormals.Load(int3(samplePixel, 0)).ba);
            if ((sampleFlags & GBufferStandardLitFlag) == 0u
                || !ReconstructFilterAt(samplePixel, samplePosition)
                || !all(isfinite(sampleNormal))
                || dot(centerNormal, sampleNormal)
                    < Filter.DirectionAndThresholds.z
                || abs(samplePosition.x - centerPosition.x)
                    > abs(centerPosition.x) * Filter.DirectionAndThresholds.w
                        + 0.01)
                continue;
        }
        weightedVisibility +=
            FilterInputVisibility.Load(int3(samplePixel, 0)) * weight;
        totalWeight += weight;
    }
    float filtered = weightedVisibility / max(totalWeight, 1.0e-6);
    return isfinite(filtered) ? saturate(filtered) : 1.0;
}
