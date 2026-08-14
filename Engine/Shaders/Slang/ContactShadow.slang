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

bool TryLoadWorldPosition(
    int2 pixel,
    int2 centerPixel,
    int2 viewportSize,
    float bReversedZ,
    out float3 worldPosition)
{
    pixel = clamp(pixel, int2(0), viewportSize - int2(1));
    if (all(pixel == centerPixel))
    {
        worldPosition = float3(0.0);
        return false;
    }

    float deviceDepth = SceneDepth.Load(int3(pixel, 0));
    bool bValid = bReversedZ > 0.5
        ? (deviceDepth > 0.001) : (deviceDepth < 0.999);
    if (!bValid)
    {
        worldPosition = float3(0.0);
        return false;
    }

    float2 uv = (float2(pixel) + 0.5) * Params.InvViewport.xy;
    worldPosition = ReconstructWorld(uv, deviceDepth);
    return true;
}

bool IsContactHit(
    float3 rayPos,
    float2 rayUV,
    float rayDepth,
    float bReversedZ,
    int2 viewportSize,
    float effectiveRayThickness,
    float3 receiverWorldPosition,
    float3 receiverPlaneNormal,
    float selfIntersectionRejectDistance)
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
    if (length(sceneWorld - rayPos) > effectiveRayThickness)
        return false;

    // A grazing ray can remain inside the finite thickness of its own receiver
    // for many pixels, producing large triangular self-shadows on walls. A real
    // contact caster must leave the receiver plane by more than a small
    // pixel-scaled tolerance; coplanar depth is the receiver itself.
    float receiverPlaneDistance = abs(dot(
        sceneWorld - receiverWorldPosition, receiverPlaneNormal));
    return receiverPlaneDistance > selfIntersectionRejectDistance;
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

    // Build receiver tangents from the nearer valid neighbor on each screen
    // axis. At a wall-floor discontinuity this keeps both tangents on the
    // receiver instead of letting a derivative quad cross onto the other
    // surface and alternate the self-intersection classification.
    float3 worldXPositive;
    float3 worldXNegative;
    float3 worldYPositive;
    float3 worldYNegative;
    bool bXPositiveValid = TryLoadWorldPosition(
        centerPixel + int2(1, 0), centerPixel, viewportSize,
        bReversedZ, worldXPositive);
    bool bXNegativeValid = TryLoadWorldPosition(
        centerPixel - int2(1, 0), centerPixel, viewportSize,
        bReversedZ, worldXNegative);
    bool bYPositiveValid = TryLoadWorldPosition(
        centerPixel + int2(0, 1), centerPixel, viewportSize,
        bReversedZ, worldYPositive);
    bool bYNegativeValid = TryLoadWorldPosition(
        centerPixel - int2(0, 1), centerPixel, viewportSize,
        bReversedZ, worldYNegative);
    float xPositiveDistance = bXPositiveValid
        ? length(worldXPositive - worldPos) : 1.0e20;
    float xNegativeDistance = bXNegativeValid
        ? length(worldXNegative - worldPos) : 1.0e20;
    float yPositiveDistance = bYPositiveValid
        ? length(worldYPositive - worldPos) : 1.0e20;
    float yNegativeDistance = bYNegativeValid
        ? length(worldYNegative - worldPos) : 1.0e20;
    float3 receiverTangentX = xPositiveDistance <= xNegativeDistance
        ? worldXPositive - worldPos : worldPos - worldXNegative;
    float3 receiverTangentY = yPositiveDistance <= yNegativeDistance
        ? worldYPositive - worldPos : worldPos - worldYNegative;
    float tangentXLength = min(xPositiveDistance, xNegativeDistance);
    float tangentYLength = min(yPositiveDistance, yNegativeDistance);
    bool bReceiverPlaneValid = tangentXLength < 1.0e19
        && tangentYLength < 1.0e19;

    // A fixed world-space thickness becomes an arbitrarily wide band in a
    // close-up. Cap hit tolerance to an eight-pixel world footprint while
    // retaining the full world thickness at ordinary viewing distances.
    float worldUnitsPerPixel = max(
        bReceiverPlaneValid ? tangentXLength : 0.001,
        bReceiverPlaneValid ? tangentYLength : 0.001);
    float effectiveRayThickness = min(
        rayThickness, max(worldUnitsPerPixel * 8.0, 0.002));
    float3 receiverPlaneNormal = bReceiverPlaneValid
        ? cross(receiverTangentX, receiverTangentY)
        : float3(0.0, 0.0, 1.0);
    float receiverPlaneNormalLength = length(receiverPlaneNormal);
    receiverPlaneNormal = receiverPlaneNormalLength > 1.0e-6
        ? receiverPlaneNormal / receiverPlaneNormalLength
        : float3(0.0, 0.0, 1.0);
    float selfIntersectionRejectDistance = clamp(
        worldUnitsPerPixel * 1.5, 0.001, 0.005);

    // Preserve the fixed step budget, but shorten its world-space extent when
    // the complete ray would project beyond the screen-space budget. Without
    // this adjustment a close camera lets one world step jump across many
    // pixels before the existing screen-distance rejection can run.
    float marchDistance = maxDistance;
    float3 fullRayEnd = worldPos + toLight * (startOffset + maxDistance);
    float4 fullRayEndClip = mul(
        Params.ViewProjection, float4(fullRayEnd, 1.0));
    if (fullRayEndClip.w > 1.0e-6)
    {
        float2 fullRayEndUV =
            (fullRayEndClip.xy / fullRayEndClip.w) * 0.5 + 0.5;
        float fullRayScreenDistancePixels = length(
            (fullRayEndUV - input.uv) / Params.InvViewport.xy);
        if (fullRayScreenDistancePixels > Params.InvViewport.w)
        {
            marchDistance *=
                Params.InvViewport.w / fullRayScreenDistancePixels;
        }
    }
    float3 marchRayEnd = worldPos + toLight * (startOffset + marchDistance);
    float4 marchRayEndClip = mul(
        Params.ViewProjection, float4(marchRayEnd, 1.0));
    if (marchRayEndClip.w > 1.0e-6)
    {
        float2 marchRayEndUV =
            (marchRayEndClip.xy / marchRayEndClip.w) * 0.5 + 0.5;
        float projectedMarchPixels = length(
            (marchRayEndUV - input.uv) / Params.InvViewport.xy);
        int projectedStepCount = (int)ceil(projectedMarchPixels / 1.5);
        stepCount = clamp(max(stepCount, projectedStepCount), stepCount, 64);
    }
    float stepDistance = marchDistance / (float)stepCount;
    float3 rayPos = worldPos + toLight * startOffset;

    // Screen-space rays cannot represent occluders after either endpoint leaves
    // the viewport. Fade the supplement through the outer 5% (bounded in
    // pixels) so this limitation does not appear as a hard cut or stretched
    // shadow when the camera approaches an object or frames it at an edge.
    float edgeFadeWidthPixels = clamp(
        (float)min(viewportSize.x, viewportSize.y) * 0.05, 16.0, 64.0);
    float2 receiverEdgeDistancePixels = min(
        input.pos.xy, float2(viewportSize) - input.pos.xy);
    float receiverEdgeFade = smoothstep(
        0.0, edgeFadeWidthPixels,
        min(receiverEdgeDistancePixels.x, receiverEdgeDistancePixels.y));

    float occlusion = 0.0;
    for (int i = 0; i < stepCount; ++i)
    {
        rayPos += toLight * stepDistance;

        float4 clip = mul(Params.ViewProjection, float4(rayPos, 1.0));
        if (clip.w <= 1.0e-6)
            continue;
        float3 ndc = clip.xyz / clip.w;
        float2 rayUV = ndc.xy * 0.5 + 0.5;
        if (rayUV.x < 0.0 || rayUV.x > 1.0 || rayUV.y < 0.0 || rayUV.y > 1.0)
            break;
        float2 screenDeltaPixels =
            (rayUV - input.uv) / Params.InvViewport.xy;
        if (dot(screenDeltaPixels, screenDeltaPixels)
            > Params.InvViewport.w * Params.InvViewport.w)
            break;

        float rayDepth = ndc.z;
        if (IsContactHit(
                rayPos, rayUV, rayDepth, bReversedZ,
                viewportSize, effectiveRayThickness,
                worldPos, receiverPlaneNormal,
                selfIntersectionRejectDistance))
        {
            // Fixed-step hit distance becomes a visible staircase when a close,
            // grazing receiver magnifies each interval. Refine the first hit
            // against the preceding miss to sub-step precision.
            float3 missRayPos = rayPos - toLight * stepDistance;
            float3 hitRayPos = rayPos;
            float2 hitRayUV = rayUV;
            for (int refinement = 0; refinement < 4; ++refinement)
            {
                float3 candidateRayPos = (missRayPos + hitRayPos) * 0.5;
                float4 candidateClip = mul(
                    Params.ViewProjection, float4(candidateRayPos, 1.0));
                if (candidateClip.w <= 1.0e-6)
                {
                    missRayPos = candidateRayPos;
                    continue;
                }
                float3 candidateNdc = candidateClip.xyz / candidateClip.w;
                float2 candidateUV = candidateNdc.xy * 0.5 + 0.5;
                bool bCandidateOnScreen = candidateUV.x >= 0.0
                    && candidateUV.x <= 1.0 && candidateUV.y >= 0.0
                    && candidateUV.y <= 1.0;
                if (bCandidateOnScreen && IsContactHit(
                        candidateRayPos, candidateUV, candidateNdc.z,
                        bReversedZ, viewportSize, effectiveRayThickness,
                        worldPos, receiverPlaneNormal,
                        selfIntersectionRejectDistance))
                {
                    hitRayPos = candidateRayPos;
                    hitRayUV = candidateUV;
                }
                else
                {
                    missRayPos = candidateRayPos;
                }
            }

            // The inner interval exists specifically to replace direct light
            // detached by the shadow-map bias. Keep it fully occluded through
            // the bias-sized thickness, then fade only the longer-range
            // screen-space supplement.
            float traveledDistance = length(hitRayPos - worldPos);
            float fadeEnd = max(maxDistance, rayThickness + 1.0e-4);
            float distanceFade = 1.0 - smoothstep(
                rayThickness, fadeEnd, traveledDistance);
            float2 rayEdgeDistancePixels = min(
                hitRayUV, 1.0 - hitRayUV) / Params.InvViewport.xy;
            float rayEdgeFade = smoothstep(
                0.0, edgeFadeWidthPixels,
                min(rayEdgeDistancePixels.x, rayEdgeDistancePixels.y));
            float screenEdgeFade = min(receiverEdgeFade, rayEdgeFade);
            occlusion = distanceFade * screenEdgeFade;
            break;
        }
    }

    if (Params.InvViewport.z > 0.5)
        return float4(occlusion, 0.0, 0.0, 1.0);

    float3 contactLoss = directionalDirect * occlusion;
    return float4(max(color.rgb - contactLoss, 0.0), color.a);
}
