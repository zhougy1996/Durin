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
    float4 HalfViewport; // xy origin, zw extent
    float4 Controls; // radius, falloff start fraction, max pixels, thickness
};

struct FGTAOFilterUniform
{
    float4 ProjectionRow0;
    float4 ProjectionRow1;
    float4 ProjectionRow2;
    float4 ProjectionRow3;
    float4 Viewport;
    float4 HalfViewport;
    float4 DirectionAndThresholds; // xy tap direction, z normal dot, w relative depth
};

[[vk::binding(0, 0)]] Texture2D<float4> GBufferNormals;
[[vk::binding(1, 0)]] Texture2D<float4> GBufferSurface;
[[vk::binding(2, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(3, 0)]] ConstantBuffer<FGTAOViewUniform> View;
[[vk::binding(10, 0)]] Texture2D<float> RawSelector;

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

bool IsInside(int2 pixel, float4 viewport)
{
    int2 viewportMin = int2(viewport.xy);
    int2 viewportMax = viewportMin + int2(viewport.zw);
    return all(pixel >= viewportMin) && all(pixel < viewportMax);
}

bool SelectRepresentative(int2 halfPixel, out int2 selectedPixel,
    out uint selector)
{
    selector = 0u;
    selectedPixel = int2(0, 0);
    float nearestDepth = 3.402823466e+38;
    int2 blockOrigin = halfPixel * 2;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        int2 candidate = blockOrigin + int2(index & 1u, index >> 1u);
        if (!IsInside(candidate, View.Viewport))
            continue;
        float4 surface = GBufferSurface.Load(int3(candidate, 0));
        uint flags = (uint)round(saturate(surface.a) * 255.0);
        float3 position;
        if ((flags & GBufferStandardLitFlag) == 0u
            || !ReconstructAt(candidate, position))
            continue;
        float depth = abs(position.x);
        if (!isfinite(depth) || depth >= nearestDepth)
            continue;
        nearestDepth = depth;
        selectedPixel = candidate;
        selector = index + 1u;
    }
    return selector != 0u;
}

bool DecodeRawRepresentative(int2 halfPixel, out int2 selectedPixel,
    out uint selector)
{
    selector = (uint)round(saturate(
        RawSelector.Load(int3(halfPixel, 0))) * 255.0);
    if (selector < 1u || selector > 4u)
    {
        selectedPixel = int2(0, 0);
        return false;
    }
    uint index = selector - 1u;
    selectedPixel = halfPixel * 2 + int2(index & 1u, index >> 1u);
    return IsInside(selectedPixel, View.Viewport);
}

float EvaluateVisibility(int2 domainPixel, bool halfResolution,
    out uint selectedSelector)
{
    int2 pixel = domainPixel;
    selectedSelector = 0u;
    if (halfResolution
        && !DecodeRawRepresentative(domainPixel, pixel, selectedSelector))
        return 1.0;
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
        * (halfResolution ? View.HalfViewport.w : View.Viewport.w) * 0.5;
    float screenRadius = min(View.Controls.z,
        View.Controls.x * projectedScale / max(abs(centerPosition.z), 1.0e-4));
    if (!isfinite(screenRadius) || screenRadius < 1.3)
        return 1.0;

    float2 noise = FixedNoise(domainPixel);
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
                int2 sampleDomain = domainPixel
                    + (side == 0u ? offset : -offset);
                float4 domainViewport = halfResolution
                    ? View.HalfViewport : View.Viewport;
                if (!IsInside(sampleDomain, domainViewport))
                    continue;
                int2 samplePixel = sampleDomain;
                uint ignoredSelector;
                if (halfResolution && !DecodeRawRepresentative(
                        sampleDomain, samplePixel, ignoredSelector))
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

[shader("fragment")]
float RawFragmentMain(VSOutput input) : SV_Target0
{
    if (!isfinite(RawSelector.Load(int3(int2(input.pos.xy), 0))))
        return 1.0;
    uint ignoredSelector;
    return EvaluateVisibility(int2(input.pos.xy), false, ignoredSelector);
}

[shader("fragment")]
float SelectorFragmentMain(VSOutput input) : SV_Target0
{
    int2 selectedPixel;
    uint selector;
    SelectRepresentative(int2(input.pos.xy), selectedPixel, selector);
    // Keep the shared raw descriptor layout explicit on every entry point.
    if (!isfinite(RawSelector.Load(int3(int2(input.pos.xy) * 2, 0))))
        selector = 0u;
    if (!all(isfinite(GBufferNormals.Load(
            int3(int2(input.pos.xy) * 2, 0)))))
        selector = 0u;
    return float(selector) / 255.0;
}

[shader("fragment")]
float HalfRawFragmentMain(VSOutput input) : SV_Target0
{
    uint selector;
    return EvaluateVisibility(int2(input.pos.xy), true, selector);
}

[[vk::binding(4, 0)]] Texture2D<float> FilterInputVisibility;
[[vk::binding(5, 0)]] Texture2D<float4> FilterGBufferNormals;
[[vk::binding(6, 0)]] Texture2D<float4> FilterGBufferSurface;
[[vk::binding(7, 0)]] Texture2D<float> FilterSceneDepth;
[[vk::binding(8, 0)]] ConstantBuffer<FGTAOFilterUniform> Filter;
[[vk::binding(9, 0)]] Texture2D<float> FilterSelector;

bool ReconstructFilterAt(int2 pixel, out float3 viewPosition)
{
    float2 local = float2(pixel) + 0.5 - Filter.Viewport.xy;
    float2 ndc = local / Filter.Viewport.zw * 2.0 - 1.0;
    return ReconstructViewPositionAnalytic(
        Filter.ProjectionRow0, Filter.ProjectionRow1,
        Filter.ProjectionRow2, Filter.ProjectionRow3,
        ndc, FilterSceneDepth.Load(int3(pixel, 0)), viewPosition);
}

bool DecodeRepresentative(int2 halfPixel, out int2 fullPixel)
{
    uint encoded = (uint)round(saturate(
        FilterSelector.Load(int3(halfPixel, 0))) * 255.0);
    if (encoded < 1u || encoded > 4u)
    {
        fullPixel = int2(0, 0);
        return false;
    }
    uint index = encoded - 1u;
    fullPixel = halfPixel * 2 + int2(index & 1u, index >> 1u);
    int2 viewportMin = int2(Filter.Viewport.xy);
    int2 viewportMax = viewportMin + int2(Filter.Viewport.zw);
    return all(fullPixel >= viewportMin) && all(fullPixel < viewportMax);
}

bool LoadFilterSurface(int2 halfPixel, out int2 fullPixel,
    out float3 position, out float3 normal)
{
    if (!DecodeRepresentative(halfPixel, fullPixel))
        return false;
    float4 surface = FilterGBufferSurface.Load(int3(fullPixel, 0));
    uint flags = (uint)round(saturate(surface.a) * 255.0);
    normal = DecodeOctahedralNormal(
        FilterGBufferNormals.Load(int3(fullPixel, 0)).ba);
    return (flags & GBufferStandardLitFlag) != 0u
        && ReconstructFilterAt(fullPixel, position)
        && all(isfinite(normal));
}

[shader("fragment")]
float BilateralFragmentMain(VSOutput input) : SV_Target0
{
    int2 pixel = int2(input.pos.xy);
    // The full and half filters share one portable pipeline layout. This
    // finite read keeps the selector slot explicit while full quality binds
    // its visibility input to the otherwise unused slot.
    if (!isfinite(FilterSelector.Load(int3(pixel, 0))))
        return 1.0;
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

[shader("fragment")]
float HalfBilateralFragmentMain(VSOutput input) : SV_Target0
{
    int2 halfPixel = int2(input.pos.xy);
    int2 centerFullPixel;
    float3 centerPosition;
    float3 centerNormal;
    if (!LoadFilterSurface(halfPixel, centerFullPixel,
            centerPosition, centerNormal))
        return 1.0;

    static const float weights[5] = {
        0.06136, 0.24477, 0.38774, 0.24477, 0.06136};
    float weightedVisibility = 0.0;
    float totalWeight = 0.0;
    int2 viewportMin = int2(Filter.HalfViewport.xy);
    int2 viewportMax = viewportMin + int2(Filter.HalfViewport.zw);
    [unroll]
    for (int tap = -2; tap <= 2; ++tap)
    {
        int2 sampleHalfPixel = halfPixel
            + int2(Filter.DirectionAndThresholds.xy) * tap;
        if (any(sampleHalfPixel < viewportMin)
            || any(sampleHalfPixel >= viewportMax))
            continue;
        float weight = weights[tap + 2];
        if (tap != 0)
        {
            int2 sampleFullPixel;
            float3 samplePosition;
            float3 sampleNormal;
            if (!LoadFilterSurface(sampleHalfPixel, sampleFullPixel,
                    samplePosition, sampleNormal)
                || dot(centerNormal, sampleNormal)
                    < Filter.DirectionAndThresholds.z
                || abs(samplePosition.x - centerPosition.x)
                    > abs(centerPosition.x) * Filter.DirectionAndThresholds.w
                        + 0.01)
                continue;
        }
        weightedVisibility += FilterInputVisibility.Load(
            int3(sampleHalfPixel, 0)) * weight;
        totalWeight += weight;
    }
    float filtered = weightedVisibility / max(totalWeight, 1.0e-6);
    return isfinite(filtered) ? saturate(filtered) : 1.0;
}

[shader("fragment")]
float ResolveFragmentMain(VSOutput input) : SV_Target0
{
    int2 pixel = int2(input.pos.xy);
    float4 surface = FilterGBufferSurface.Load(int3(pixel, 0));
    uint flags = (uint)round(saturate(surface.a) * 255.0);
    float3 centerPosition;
    float3 centerNormal = DecodeOctahedralNormal(
        FilterGBufferNormals.Load(int3(pixel, 0)).ba);
    if ((flags & GBufferStandardLitFlag) == 0u
        || !ReconstructFilterAt(pixel, centerPosition)
        || !all(isfinite(centerNormal)))
        return 1.0;

    float2 halfCoordinate = (float2(pixel) + 0.5) * 0.5 - 0.5;
    int2 halfBase = int2(floor(halfCoordinate));
    float2 fraction = frac(halfCoordinate);
    int2 viewportMin = int2(Filter.HalfViewport.xy);
    int2 viewportMax = viewportMin + int2(Filter.HalfViewport.zw);
    float weightedVisibility = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (uint index = 0u; index < 4u; ++index)
    {
        int2 offset = int2(index & 1u, index >> 1u);
        int2 halfPixel = halfBase + offset;
        if (any(halfPixel < viewportMin) || any(halfPixel >= viewportMax))
            continue;
        int2 representativePixel;
        float3 representativePosition;
        float3 representativeNormal;
        if (!LoadFilterSurface(halfPixel, representativePixel,
                representativePosition, representativeNormal)
            || dot(centerNormal, representativeNormal)
                < Filter.DirectionAndThresholds.z
            || abs(representativePosition.x - centerPosition.x)
                > abs(centerPosition.x) * Filter.DirectionAndThresholds.w
                    + 0.01)
            continue;
        float2 axisWeight = float2(
            offset.x == 0 ? 1.0 - fraction.x : fraction.x,
            offset.y == 0 ? 1.0 - fraction.y : fraction.y);
        float weight = axisWeight.x * axisWeight.y;
        weightedVisibility += FilterInputVisibility.Load(
            int3(halfPixel, 0)) * weight;
        totalWeight += weight;
    }
    if (totalWeight <= 1.0e-6)
        return 1.0;
    float resolved = weightedVisibility / totalWeight;
    return isfinite(resolved) ? saturate(resolved) : 1.0;
}
