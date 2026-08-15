module ContactShadow;

import Material.GBufferDecode;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

struct ContactShadowUniform
{
    float4x4 InverseViewProjection;
    float4x4 ViewProjection;
    float4 ToLightMaxDistance;
    float4 ThicknessStepsOffsetReversedZ;
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

bool IsContactHit(float3 rayPosition, float2 rayLocalUV, float rayDepth,
    int2 targetSize, int2 viewportSize, float rayThickness)
{
    int2 rayPixel = clamp(int2(Params.Viewport.zw)
        + int2(rayLocalUV * float2(viewportSize)), int2(0), targetSize - int2(1));
    float sceneDepth = SceneDepth.Load(int3(rayPixel, 0));
    bool reversedZ = Params.ThicknessStepsOffsetReversedZ.w > 0.5;
    bool sceneValid = reversedZ ? sceneDepth > 0.001 : sceneDepth < 0.999;
    if (!sceneValid)
        return false;
    float depthDifference = reversedZ
        ? sceneDepth - rayDepth : rayDepth - sceneDepth;
    if (depthDifference <= 0.0)
        return false;
    float2 sceneLocalUV = (float2(rayPixel) + 0.5 - Params.Viewport.zw)
        * Params.Viewport.xy;
    float3 separation = ReconstructWorld(sceneLocalUV, sceneDepth) - rayPosition;
    return dot(separation, separation) <= rayThickness * rayThickness;
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
    bool reversedZ = Params.ThicknessStepsOffsetReversedZ.w > 0.5;
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
    float rayThickness = Params.ThicknessStepsOffsetReversedZ.x;
    int stepCount = max(1, (int)Params.ThicknessStepsOffsetReversedZ.y);
    float startOffset = Params.ThicknessStepsOffsetReversedZ.z;
    int2 viewportSize = max(int2(round(rcp(Params.Viewport.xy))), int2(1));
    uint targetWidth;
    uint targetHeight;
    SceneDepth.GetDimensions(targetWidth, targetHeight);
    int2 targetSize = int2(targetWidth, targetHeight);
    float stepDistance = max(maxDistance - startOffset, 1.0e-4) / (float)stepCount;
    float rayDistance = startOffset + stepDistance * 0.5;
    float3 rayPosition = worldPosition + toLight * rayDistance;
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
        if (IsContactHit(rayPosition, rayLocalUV, ndc.z,
                targetSize, viewportSize, rayThickness))
        {
            float distanceFade = 1.0 - smoothstep(
                startOffset, maxDistance, rayDistance);
            float screenFade = 1.0 - smoothstep(Params.Trace.x * 0.75,
                Params.Trace.x, sqrt(screenDistanceSquared));
            occlusion = distanceFade * screenFade * facingFade;
            break;
        }
        rayDistance += stepDistance;
        rayPosition += toLight * stepDistance;
    }
    return 1.0 - saturate(occlusion);
}
