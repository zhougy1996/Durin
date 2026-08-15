module ContactShadow;

import Material.GBufferDecode;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

struct ContactShadowUniform
{
    float4x4 InverseViewProjection;
    float4x4 ViewProjection;
    float4 ToLightMaxDistance;
    float4 SurfaceStepsMinBiasReversedZ;
    float4 Viewport; // xy = inverse local extent, zw = absolute origin
    float4 Trace;    // x = max screen displacement in pixels
};

[[vk::binding(0, 0)]] Texture2D<float4> GBufferMaterial;
[[vk::binding(1, 0)]] Texture2D<float4> GBufferNormals;
[[vk::binding(2, 0)]] Texture2D<float4> GBufferSurface;
[[vk::binding(3, 0)]] Texture2D<float3> GBufferEmissive;
[[vk::binding(4, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(5, 0)]] ConstantBuffer<ContactShadowUniform> Params;

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
    float2 rayLocalUV, int2 targetSize, int2 viewportSize,
    float3 receiverPosition, float3 receiverNormal, float receiverRadius)
{
    int2 rayPixel = clamp(int2(Params.Viewport.zw)
        + int2(rayLocalUV * float2(viewportSize)), int2(0),
        targetSize - int2(1));
    float sceneDepth = SceneDepth.Load(int3(rayPixel, 0));
    bool reversedZ = Params.SurfaceStepsMinBiasReversedZ.w > 0.5;
    bool sceneValid = reversedZ ? sceneDepth > 0.001 : sceneDepth < 0.999;
    if (!sceneValid)
        return false;
    float2 sceneLocalUV = (float2(rayPixel) + 0.5 - Params.Viewport.zw)
        * Params.Viewport.xy;
    float3 surfacePosition = ReconstructWorld(sceneLocalUV, sceneDepth);
    float3 surfaceNormal = DecodeOctahedralNormal(
        GBufferNormals.Load(int3(rayPixel, 0)).ba);
    float surfaceRadius = PixelWorldRadius(sceneLocalUV, sceneDepth,
        surfacePosition, surfaceNormal)
        * Params.SurfaceStepsMinBiasReversedZ.x;

    // A parallel, near-coplanar sample is the receiver surface represented by
    // another depth texel, not an independent blocker.
    float receiverAlignment = abs(dot(surfaceNormal, receiverNormal));
    float receiverPlaneDistance = abs(dot(
        surfacePosition - receiverPosition, receiverNormal));
    if (receiverAlignment >= 0.98
        && receiverPlaneDistance <= receiverRadius + surfaceRadius)
        return false;

    // Treat the depth texel as a finite oriented surfel. A blocker exists only
    // when this march segment crosses its plane inside the pixel footprint.
    float startDistance = dot(segmentStart - surfacePosition, surfaceNormal);
    float endDistance = dot(segmentEnd - surfacePosition, surfaceNormal);
    float distanceDelta = startDistance - endDistance;
    if (startDistance * endDistance > 0.0 || abs(distanceDelta) <= 1.0e-7)
        return false;
    float segmentT = startDistance / distanceDelta;
    if (segmentT < 0.0 || segmentT > 1.0)
        return false;
    float3 intersection = lerp(segmentStart, segmentEnd, segmentT);
    float3 footprintDelta = intersection - surfacePosition;
    footprintDelta -= surfaceNormal * dot(footprintDelta, surfaceNormal);
    return dot(footprintDelta, footprintDelta) <= surfaceRadius * surfaceRadius;
}

[shader("fragment")]
float ContactVisibilityFragmentMain(VSOutput input) : SV_Target0
{
    int2 pixel = int2(input.pos.xy);
    FGBufferRecord receiver = DecodeGBuffer(
        GBufferMaterial.Load(int3(pixel, 0)),
        GBufferNormals.Load(int3(pixel, 0)),
        GBufferSurface.Load(int3(pixel, 0)),
        GBufferEmissive.Load(int3(pixel, 0)));
    if ((receiver.flags & GBufferStandardLitFlag) == 0u)
        return 1.0;
    float centerDepth = SceneDepth.Load(int3(pixel, 0));
    bool reversedZ = Params.SurfaceStepsMinBiasReversedZ.w > 0.5;
    if (reversedZ ? centerDepth <= 0.001 : centerDepth >= 0.999)
        return 1.0;

    float3 toLight = Params.ToLightMaxDistance.xyz;
    float facingFade = smoothstep(
        0.02, 0.20, saturate(dot(receiver.geometricNormal, toLight)));
    if (facingFade <= 0.0)
        return 1.0;

    float2 localUV = (float2(pixel) + 0.5 - Params.Viewport.zw)
        * Params.Viewport.xy;
    float3 worldPosition = ReconstructWorld(localUV, centerDepth);
    float maxDistance = Params.ToLightMaxDistance.w;
    float receiverRadius = PixelWorldRadius(localUV, centerDepth,
        worldPosition, receiver.geometricNormal)
        * Params.SurfaceStepsMinBiasReversedZ.x;
    int stepCount = max(1, (int)Params.SurfaceStepsMinBiasReversedZ.y);
    float normalBias = max(Params.SurfaceStepsMinBiasReversedZ.z,
        receiverRadius * 0.5);
    normalBias = min(normalBias, maxDistance * 0.05);
    int2 viewportSize = max(int2(round(rcp(Params.Viewport.xy))), int2(1));
    uint targetWidth;
    uint targetHeight;
    SceneDepth.GetDimensions(targetWidth, targetHeight);
    int2 targetSize = int2(targetWidth, targetHeight);
    float stepDistance = max(maxDistance, 1.0e-4) / (float)stepCount;
    float rayDistance = stepDistance;
    float3 segmentStart = worldPosition + receiver.geometricNormal * normalBias;
    float3 rayPosition = segmentStart + toLight * stepDistance;
    float occlusion = 0.0;
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        float4 clip = mul(Params.ViewProjection, float4(rayPosition, 1.0));
        if (clip.w <= 1.0e-6)
            break;
        float3 ndc = clip.xyz / clip.w;
        float2 rayLocalUV = ndc.xy * 0.5 + 0.5;
        if (any(rayLocalUV < 0.0) || any(rayLocalUV > 1.0))
            break;
        float2 screenDeltaPixels = (rayLocalUV - localUV) / Params.Viewport.xy;
        float screenDistanceSquared = dot(screenDeltaPixels, screenDeltaPixels);
        if (screenDistanceSquared > Params.Trace.x * Params.Trace.x)
            break;
        if (IsContactHit(segmentStart, rayPosition, rayLocalUV,
                targetSize, viewportSize, worldPosition,
                receiver.geometricNormal, receiverRadius))
        {
            float distanceFade = 1.0 - smoothstep(
                0.0, maxDistance, rayDistance);
            float screenFade = 1.0 - smoothstep(Params.Trace.x * 0.75,
                Params.Trace.x, sqrt(screenDistanceSquared));
            occlusion = distanceFade * screenFade * facingFade;
            break;
        }
        rayDistance += stepDistance;
        segmentStart = rayPosition;
        rayPosition += toLight * stepDistance;
    }
    return 1.0 - saturate(occlusion);
}
