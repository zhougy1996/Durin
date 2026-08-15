// Fullscreen screen-space contact-shadow pass. Reads the Scene Color depth,
// reconstructs world position, and marches toward the selected directional
// light to darken short-range contact that the shadow map's bias detaches.

struct VSInput
{
    float2 pos : POSITION;
    float2 uv  : TEXCOORD;
};

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD;
};

struct ContactShadowUniform
{
    float4x4 InverseViewProjection;
    float4x4 ViewProjection;
    float4 ToLightMaxDistance;   // xyz = normalized toward-light, w = max world distance
    float4 ThicknessStepsDebug;  // x = world thickness, y = step count, z = start offset, w = bReversedZ
    float4 InvViewport;          // xy = 1/viewport, z = bShowDebug, w = max screen distance in pixels
};

[[vk::binding(0, 0)]] Texture2D<float4> SceneColor;
[[vk::binding(1, 0)]] SamplerState SceneColorSampler;
[[vk::binding(2, 0)]] Texture2D<float4> DirectionalDirect;
[[vk::binding(3, 0)]] Texture2D<float> SceneDepth;
[[vk::binding(4, 0)]] ConstantBuffer<ContactShadowUniform> Params;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

float3 ReconstructWorld(float2 uv, float deviceDepth)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 clip = float4(ndc, deviceDepth, 1.0);
    float4 worldH = mul(Params.InverseViewProjection, clip);
    return worldH.xyz / worldH.w;
}

bool IsContactHit(
    float3 rayPos,
    float2 rayUV,
    float rayDepth,
    float bReversedZ,
    int2 viewportSize,
    float rayThickness)
{
    int2 rayPixel = clamp(
        int2(rayUV * float2(viewportSize)),
        int2(0), viewportSize - int2(1));
    float2 sceneUV =
        (float2(rayPixel) + 0.5) * Params.InvViewport.xy;
    float sceneDepth = SceneDepth.Load(int3(rayPixel, 0));
    bool bSceneValid = bReversedZ > 0.5
        ? (sceneDepth > 0.001) : (sceneDepth < 0.999);
    if (!bSceneValid)
        return false;

    float depthDiff = bReversedZ > 0.5
        ? (sceneDepth - rayDepth) : (rayDepth - sceneDepth);
    if (depthDiff <= 0.0)
        return false;

    float3 sceneWorld = ReconstructWorld(sceneUV, sceneDepth);
    float3 separation = sceneWorld - rayPos;
    return dot(separation, separation) <= rayThickness * rayThickness;
}

[shader("fragment")]
float4 ContactShadowFragmentMain(VSOutput input) : SV_Target
{
    float4 color = SceneColor.Sample(SceneColorSampler, input.uv);
    int2 centerPixel = int2(input.pos.xy);
    float centerDepth = SceneDepth.Load(int3(centerPixel, 0));
    float3 directionalDirect = DirectionalDirect.Load(int3(centerPixel, 0)).rgb;
    if (max(directionalDirect.r, max(directionalDirect.g, directionalDirect.b)) <= 1.0e-5)
        return Params.InvViewport.z > 0.5
            ? float4(0.0, 0.0, 0.0, 1.0) : color;

    float bReversedZ = Params.ThicknessStepsDebug.w;
    bool bValid = bReversedZ > 0.5 ? (centerDepth > 0.001) : (centerDepth < 0.999);
    if (!bValid)
        return Params.InvViewport.z > 0.5
            ? float4(0.0, 0.0, 0.0, 1.0) : color;

    float3 worldPos = ReconstructWorld(input.uv, centerDepth);
    float3 toLight = Params.ToLightMaxDistance.xyz;
    float maxDistance = Params.ToLightMaxDistance.w;
    float rayThickness = Params.ThicknessStepsDebug.x;
    int stepCount = max(1, (int)Params.ThicknessStepsDebug.y);
    float startOffset = Params.ThicknessStepsDebug.z;

    int2 viewportSize = max(
        int2(round(1.0 / Params.InvViewport.xy)), int2(1));

    // This is a short-range detail trace, not a replacement shadow. Keep one
    // fixed loop budget on every platform and sample interval midpoints so the
    // first useful query is close to the receiver without querying the receiver
    // itself. The start bias and finite thickness are the only self-hit guards.
    float marchDistance = max(maxDistance - startOffset, 1.0e-4);
    float stepDistance = marchDistance / (float)stepCount;
    float rayDistance = startOffset + stepDistance * 0.5;
    float3 rayPos = worldPos + toLight * rayDistance;

    float occlusion = 0.0;
    for (int i = 0; i < stepCount; ++i)
    {
        float4 clip = mul(Params.ViewProjection, float4(rayPos, 1.0));
        if (clip.w <= 1.0e-6)
            break;
        float3 ndc = clip.xyz / clip.w;
        float2 rayUV = ndc.xy * 0.5 + 0.5;
        if (rayUV.x < 0.0 || rayUV.x > 1.0 || rayUV.y < 0.0 || rayUV.y > 1.0)
            break;
        float2 screenDeltaPixels =
            (rayUV - input.uv) / Params.InvViewport.xy;
        float screenDistanceSquared = dot(
            screenDeltaPixels, screenDeltaPixels);
        if (screenDistanceSquared
            > Params.InvViewport.w * Params.InvViewport.w)
            break;

        float rayDepth = ndc.z;
        if (IsContactHit(
                rayPos, rayUV, rayDepth, bReversedZ,
                viewportSize, rayThickness))
        {
            float fadeEnd = max(maxDistance, startOffset + 1.0e-4);
            float distanceFade = 1.0 - smoothstep(
                startOffset, fadeEnd, rayDistance);
            float screenFadeStart = Params.InvViewport.w * 0.75;
            float screenFade = 1.0 - smoothstep(
                screenFadeStart, Params.InvViewport.w,
                sqrt(screenDistanceSquared));
            occlusion = distanceFade * screenFade;
            break;
        }

        rayDistance += stepDistance;
        rayPos += toLight * stepDistance;
    }

    if (Params.InvViewport.z > 0.5)
        return float4(occlusion, 0.0, 0.0, 1.0);

    float3 contactLoss = directionalDirect * occlusion;
    return float4(max(color.rgb - contactLoss, 0.0), color.a);
}
