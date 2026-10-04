module ContactShadow;

import Material.GBufferDecode;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

struct ContactShadowUniform
{
    float4x4 InverseViewProjection;
    float4x4 ViewProjection;
    float4 ToLightMaxDistance;
    float4 SurfaceBiasReversedZ; // x = footprint dilation, y = min bias, z = reversed Z
    float4 Viewport; // xy = inverse local extent, zw = absolute origin
    float4 Trace;    // x = max screen displacement in pixels
};

[[vk::binding(0, 0)]] Texture2D<float4> GBufferMaterial;
[[vk::binding(1, 0)]] Texture2D<float4> GBufferNormals;
[[vk::binding(2, 0)]] Texture2D<float4> GBufferSurface;
[[vk::binding(3, 0)]] Texture2D<float3> GBufferEmissive;
[[vk::binding(4, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(5, 0)]] ConstantBuffer<ContactShadowUniform> Params;
[[vk::binding(6, 0)]] RWTexture2D<unorm float> ContactVisibilityOutput;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

float3 ReconstructWorld(float2 localUV, float deviceDepth)
{
    float4 worldH = mul(Params.InverseViewProjection,
        float4(localUV * 2.0 - 1.0, deviceDepth, 1.0));
    return worldH.xyz / worldH.w;
}

float PixelWorldRadius(float2 localUV, float deviceDepth, float3 worldPosition,
    float3 surfaceNormal)
{
    float2 texel = Params.Viewport.xy;
    float2 xUV = localUV + float2(
        localUV.x + texel.x <= 1.0 ? texel.x : -texel.x, 0.0);
    float2 yUV = localUV + float2(
        0.0, localUV.y + texel.y <= 1.0 ? texel.y : -texel.y);
    float3 xSpan = ReconstructWorld(xUV, deviceDepth) - worldPosition;
    float3 ySpan = ReconstructWorld(yUV, deviceDepth) - worldPosition;
    xSpan -= surfaceNormal * dot(xSpan, surfaceNormal);
    ySpan -= surfaceNormal * dot(ySpan, surfaceNormal);
    return 0.5 * length(float2(length(xSpan), length(ySpan)));
}

bool IsContactHit(float3 segmentStart, float3 segmentEnd,
    int2 rayPixel, float3 receiverPosition, float3 receiverNormal,
    float receiverRadius, out float hitFraction)
{
    hitFraction = 0.0;
    float sceneDepth = SceneDepth.Load(int3(rayPixel, 0));
    bool reversedZ = Params.SurfaceBiasReversedZ.z > 0.5;
    bool sceneValid = reversedZ ? sceneDepth > 0.001 : sceneDepth < 0.999;
    if (!sceneValid)
        return false;
    float2 sceneLocalUV = (float2(rayPixel) + 0.5 - Params.Viewport.zw)
        * Params.Viewport.xy;
    float3 surfacePosition = ReconstructWorld(sceneLocalUV, sceneDepth);
    float3 surfaceNormal = DecodeOctahedralNormal(
        GBufferNormals.Load(int3(rayPixel, 0)).ba);

    // A parallel, near-coplanar sample is the receiver surface represented by
    // another depth texel, not an independent blocker.
    float receiverAlignment = abs(dot(surfaceNormal, receiverNormal));
    float receiverPlaneDistance = abs(dot(
        surfacePosition - receiverPosition, receiverNormal));
    float coplanarTolerance = max(Params.SurfaceBiasReversedZ.y,
        receiverRadius * 0.05);
    if (receiverAlignment >= 0.98
        && receiverPlaneDistance <= coplanarTolerance)
        return false;

    // This segment projects entirely inside rayPixel. Crossing the sampled
    // plane therefore intersects its finite pixel footprint, including a
    // foreshortened wall. A world-space disk underestimates that coverage.
    float startDistance = dot(segmentStart - surfacePosition, surfaceNormal);
    float endDistance = dot(segmentEnd - surfacePosition, surfaceNormal);
    float distanceDelta = startDistance - endDistance;
    if (startDistance * endDistance > 0.0 || abs(distanceDelta) <= 1.0e-7)
        return false;
    float segmentT = startDistance / distanceDelta;
    if (segmentT < 0.0 || segmentT > 1.0)
        return false;
    hitFraction = segmentT;
    return true;
}

float CalculateContactVisibility(int2 pixel)
{
    FGBufferRecord receiver = DecodeGBuffer(
        GBufferMaterial.Load(int3(pixel, 0)),
        GBufferNormals.Load(int3(pixel, 0)),
        GBufferSurface.Load(int3(pixel, 0)),
        GBufferEmissive.Load(int3(pixel, 0)));
    if ((receiver.flags & GBufferStandardLitFlag) == 0u)
        return 1.0;
    float centerDepth = SceneDepth.Load(int3(pixel, 0));
    bool reversedZ = Params.SurfaceBiasReversedZ.z > 0.5;
    if (reversedZ ? centerDepth <= 0.001 : centerDepth >= 0.999)
        return 1.0;

    float3 toLight = Params.ToLightMaxDistance.xyz;
    float receiverFacing = dot(receiver.geometricNormal, toLight);
    if (receiverFacing <= 0.0)
        return 1.0;

    float2 localUV = (float2(pixel) + 0.5 - Params.Viewport.zw)
        * Params.Viewport.xy;
    float3 worldPosition = ReconstructWorld(localUV, centerDepth);
    float maxDistance = Params.ToLightMaxDistance.w;
    float receiverRadius = PixelWorldRadius(localUV, centerDepth,
        worldPosition, receiver.geometricNormal)
        * Params.SurfaceBiasReversedZ.x;
    float normalBias = max(Params.SurfaceBiasReversedZ.y,
        receiverRadius * 0.5);
    normalBias = min(normalBias, maxDistance * 0.05);
    int2 viewportSize = max(int2(round(rcp(Params.Viewport.xy))), int2(1));
    float3 rayStart = worldPosition + receiver.geometricNormal * normalBias;
    float3 rayEnd = rayStart + toLight * maxDistance;
    float4 clipStart = mul(Params.ViewProjection, float4(rayStart, 1.0));
    float4 clipEnd = mul(Params.ViewProjection, float4(rayEnd, 1.0));
    if (!all(isfinite(clipStart)) || !all(isfinite(clipEnd)))
        return 1.0;

    // Clip in homogeneous space before division, including a ray towards the
    // camera. All subsequent pixel intervals then have positive reciprocal W.
    float3 startPlanes = float3(clipStart.w - 1.0e-6,
        clipStart.z, clipStart.w - clipStart.z);
    float3 endPlanes = float3(clipEnd.w - 1.0e-6,
        clipEnd.z, clipEnd.w - clipEnd.z);
    if (any(startPlanes < 0.0))
        return 1.0;
    float worldEndFraction = 1.0;
    for (int plane = 0; plane < 3; ++plane)
        if (endPlanes[plane] < 0.0)
            worldEndFraction = min(worldEndFraction,
                startPlanes[plane] / (startPlanes[plane] - endPlanes[plane]));
    rayEnd = lerp(rayStart, rayEnd, worldEndFraction);
    clipEnd = lerp(clipStart, clipEnd, worldEndFraction);
    float2 startPixel = (clipStart.xy / clipStart.w * 0.5 + 0.5)
        * float2(viewportSize);
    float2 endPixel = (clipEnd.xy / clipEnd.w * 0.5 + 0.5)
        * float2(viewportSize);
    float2 screenDelta = endPixel - startPixel;
    if (any(startPixel < 0.0) || any(startPixel >= float2(viewportSize)))
        return 1.0;

    // Bound the swept screen length, rather than skipping a long world step
    // after it has already jumped across a thin blocker.
    float screenLength = length(screenDelta);
    float endFraction = screenLength > 1.0e-6
        ? min(1.0, Params.Trace.x / screenLength) : 1.0;
    for (int axis = 0; axis < 2; ++axis)
    {
        if (screenDelta[axis] > 1.0e-6)
            endFraction = min(endFraction,
                (float(viewportSize[axis]) - startPixel[axis]) / screenDelta[axis]);
        else if (screenDelta[axis] < -1.0e-6)
            endFraction = min(endFraction, -startPixel[axis] / screenDelta[axis]);
    }

    int2 cell = int2(floor(startPixel));
    int2 cellStep = int2(screenDelta.x >= 0.0 ? 1 : -1,
        screenDelta.y >= 0.0 ? 1 : -1);
    float2 nextBoundary = float2(1.0e30);
    float2 boundaryStride = float2(1.0e30);
    for (int axis = 0; axis < 2; ++axis)
    {
        if (abs(screenDelta[axis]) > 1.0e-6)
        {
            float boundary = float(cell[axis] + (cellStep[axis] > 0 ? 1 : 0));
            nextBoundary[axis] = (boundary - startPixel[axis]) / screenDelta[axis];
            boundaryStride[axis] = rcp(abs(screenDelta[axis]));
        }
    }

    float screenFraction = 0.0;
    float3 segmentStart = rayStart;
    // A 48-pixel line crosses at most ceil(|dx|)+ceil(|dy|)+1 cells.
    // 2*ceil(budget)+3 also covers a start exactly on a pixel boundary.
    int maxCells = int(ceil(Params.Trace.x)) * 2 + 3;
    for (int cellIndex = 0; cellIndex < maxCells; ++cellIndex)
    {
        if (any(cell < 0) || any(cell >= viewportSize))
            break;
        float nextFraction = min(endFraction,
            min(nextBoundary.x, nextBoundary.y));
        // Projected fractions are not world-distance fractions in perspective.
        float worldFraction = nextFraction * clipStart.w
            / lerp(clipEnd.w, clipStart.w, nextFraction);
        float3 segmentEnd = lerp(rayStart, rayEnd, worldFraction);
        float hitFraction;
        if (nextFraction > screenFraction && IsContactHit(segmentStart,
                segmentEnd, int2(Params.Viewport.zw) + cell, worldPosition,
                receiver.geometricNormal, receiverRadius, hitFraction))
        {
            float3 hitPosition = lerp(segmentStart, segmentEnd, hitFraction);
            float rayDistance = dot(hitPosition - rayStart, toLight);
            float4 hitClip = mul(Params.ViewProjection, float4(hitPosition, 1.0));
            float2 hitPixel = (hitClip.xy / hitClip.w * 0.5 + 0.5)
                * float2(viewportSize);
            float screenDistance = length(hitPixel - startPixel);
            float distanceFade = 1.0 - smoothstep(0.0, maxDistance, rayDistance);
            float screenFade = 1.0 - smoothstep(Params.Trace.x * 0.75,
                Params.Trace.x, screenDistance);
            return 1.0 - saturate(distanceFade * screenFade);
        }
        if (nextFraction >= endFraction)
            break;
        // Advance both axes at a corner; zero-length corner touches do not
        // cover a depth texel and must not manufacture an occluder.
        bool2 crossed = nextBoundary <= nextFraction;
        if (crossed.x)
        {
            cell.x += cellStep.x;
            nextBoundary.x += boundaryStride.x;
        }
        if (crossed.y)
        {
            cell.y += cellStep.y;
            nextBoundary.y += boundaryStride.y;
        }
        screenFraction = nextFraction;
        segmentStart = segmentEnd;
    }
    return 1.0;
}

[shader("fragment")]
float ContactVisibilityFragmentMain(VSOutput input) : SV_Target0
{
    return CalculateContactVisibility(int2(input.pos.xy));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void ContactVisibilityComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint targetWidth;
    uint targetHeight;
    ContactVisibilityOutput.GetDimensions(targetWidth, targetHeight);
    if (dispatchThreadId.x >= targetWidth || dispatchThreadId.y >= targetHeight)
        return;
    int2 pixel = int2(dispatchThreadId.xy);
    int2 viewportOrigin = int2(Params.Viewport.zw);
    int2 viewportSize = max(int2(round(rcp(Params.Viewport.xy))), int2(1));
    const bool insideViewport = all(pixel >= viewportOrigin)
        && all(pixel < viewportOrigin + viewportSize);
    ContactVisibilityOutput[pixel] = insideViewport
        ? CalculateContactVisibility(pixel) : 1.0;
}
