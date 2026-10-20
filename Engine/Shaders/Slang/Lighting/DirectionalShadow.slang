module DirectionalShadow;

// Numeric values match EDirectionalShadowDiagnosticMode in SceneView.h.
// These constants describe the existing CPU uniform encoding.
static const uint ShadowDiagnosticLit = 0u;
static const uint ShadowDiagnosticShadowDepthCoverage = 1u;
static const uint ShadowDiagnosticReceiverUnbiased = 2u;
static const uint ShadowDiagnosticReceiverBiased = 3u;
static const uint ShadowDiagnosticReceiverNormalOffset = 4u;
static const uint ShadowDiagnosticTexelGrid = 5u;
static const uint ShadowDiagnosticBiasContributions = 6u;
static const uint ShadowDiagnosticClassification = 7u;
static const uint ShadowDiagnosticFilterFootprint = 8u;
static const uint ShadowDiagnosticFilterTapValidity = 9u;
static const uint ShadowDiagnosticFilterDifference = 10u;
static const uint ShadowDiagnosticCascadeIndex = 11u;
static const uint ShadowDiagnosticCascadeTransition = 12u;
static const uint ShadowDiagnosticCascadeCoverage = 13u;
static const uint ShadowDiagnosticCascadeDifference = 14u;

// Numeric values match EDirectionalShadowFilterQuality in SceneView.h.
// These constants describe the existing CPU uniform encoding.
static const uint ShadowFilterLow = 0u;
static const uint ShadowFilterMedium = 1u;
static const uint ShadowFilterHigh = 2u;

public struct FDirectionalShadowSample
{
    public float attenuation;
    public float3 diagnosticColor;
};

public struct FDirectionalShadowCascadeUniform
{
    public float4x4 WorldToShadow;
    public float4 TexelBias; // xy = world texel size, z = comparison bias, w = normal bias
    public float4 RasterBias;
    public float4 Filter; // xy = inverse texture extent, z = quality, w = footprint radius
    public float4 ValidRegion;
};

float3 ProjectShadow(float3 worldPosition, float4x4 worldToShadow)
{
    float4 projected = mul(worldToShadow, float4(worldPosition, 1.0));
    if (!all(isfinite(projected)) || abs(projected.w) <= 1.0e-8)
        return float3(-1.0);
    return projected.xyz / projected.w;
}

bool IsValidShadowCoordinate(float3 coordinate)
{
    return all(isfinite(coordinate))
        && all(coordinate.xy >= float2(0.0))
        && all(coordinate.xy <= float2(1.0))
        && coordinate.z >= 0.0 && coordinate.z <= 1.0;
}

float SampleDirectionalShadowKernel(
    float3 coordinate,
    float4 filter,
    float4 validRegion,
    uint layer,
    Texture2DArray<float> shadowTexture,
    SamplerComparisonState shadowSampler,
    out float validWeight)
{
    validWeight = 0.0;
    if (!IsValidShadowCoordinate(coordinate))
        return 1.0;
    uint quality = min((uint)(filter.z + 0.5), ShadowFilterHigh);
    int radius = quality == ShadowFilterLow ? 0 : (quality == ShadowFilterMedium ? 1 : 2);
    float normalization = quality == ShadowFilterLow ? 1.0
        : (quality == ShadowFilterMedium ? 1.0 / 16.0 : 1.0 / 81.0);
    float accumulated = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            if (abs(x) > radius || abs(y) > radius)
                continue;
            int axisWeightX = quality == ShadowFilterLow ? 1
                : (quality == ShadowFilterMedium ? (x == 0 ? 2 : 1) : (3 - abs(x)));
            int axisWeightY = quality == ShadowFilterLow ? 1
                : (quality == ShadowFilterMedium ? (y == 0 ? 2 : 1) : (3 - abs(y)));
            float weight = float(axisWeightX * axisWeightY);
            float2 tap = coordinate.xy + float2(x, y) * filter.xy;
            float2 halfTexel = filter.xy * 0.5;
            bool tapValid = all(isfinite(tap))
                && all(tap >= max(halfTexel, validRegion.xy))
                && all(tap <= min(1.0 - halfTexel, validRegion.zw));
            if (tapValid)
            {
                accumulated += weight * shadowTexture.SampleCmpLevelZero(
                    shadowSampler, float3(tap, float(layer)), coordinate.z);
                validWeight += weight;
            }
            else
                accumulated += weight;
        }
    }
    validWeight *= normalization;
    return accumulated * normalization;
}

FDirectionalShadowSample EvaluateDirectionalShadowCascade(
    float3 worldPosition,
    float3 geometricNormal,
    float4 control,
    float3 lightDirection,
    FDirectionalShadowCascadeUniform cascade,
    uint layer,
    Texture2DArray<float> shadowTexture,
    SamplerComparisonState shadowSampler)
{
    FDirectionalShadowSample result;
    result.attenuation = 1.0;
    result.diagnosticColor = float3(1.0);
    if (control.x < 0.5)
        return result;
    float4x4 worldToShadow = cascade.WorldToShadow;
    float4 texelBias = cascade.TexelBias;
    float4 rasterBias = cascade.RasterBias;
    float4 filter = cascade.Filter;
    float4 validRegion = cascade.ValidRegion;
    float3 coordinate = ProjectShadow(worldPosition, worldToShadow);
    if (!IsValidShadowCoordinate(coordinate))
    {
        result.diagnosticColor = float3(0.1, 0.25, 1.0);
        return result;
    }
    uint width;
    uint height;
    uint layers;
    shadowTexture.GetDimensions(width, height, layers);
    filter.xy = 1.0 / float2(width, height);
	// Comparison bias and normal displacement have independent light controls.
	// Normal displacement uses geometry rather than the material's mapped normal.
	float normalWorld = 0.0;
	bool totalClamped = false;
	float3 receiverCoordinate = coordinate - float3(0.0, 0.0, texelBias.z);
	float3 finalCoordinate = receiverCoordinate;
	if (texelBias.w > 0.0 && all(isfinite(geometricNormal))
		&& dot(geometricNormal, geometricNormal) > 1.0e-8)
	{
		float3 normal = normalize(geometricNormal);
		float cosine = saturate(abs(dot(normal, -normalize(lightDirection))));
		normalWorld = texelBias.w * sqrt(max(0.0, 1.0 - cosine * cosine));
		float normalLimit = min(0.75 * max(texelBias.x, texelBias.y), 0.08);
		totalClamped = texelBias.w >= normalLimit;
		finalCoordinate = ProjectShadow(worldPosition + normal * normalWorld,
			worldToShadow) - float3(0.0, 0.0, texelBias.z);
	}
	float validTapWeight = 0.0;
	float finalComparison = SampleDirectionalShadowKernel(
		finalCoordinate, filter, validRegion, layer,
		shadowTexture, shadowSampler, validTapWeight);
	result.attenuation = finalComparison;

    // Production needs only the filtered comparison. Keep exact depth reads
    // behind the uniform diagnostic mode, including the cascade-only modes.
    uint mode = (uint)(control.y + 0.5);
    if (mode == ShadowDiagnosticLit)
        return result;
    float storedDepth = 1.0;
    if (mode == ShadowDiagnosticShadowDepthCoverage
        || mode == ShadowDiagnosticReceiverUnbiased
        || mode == ShadowDiagnosticClassification)
    {
        int2 texel = clamp(int2(coordinate.xy * float2(width, height)),
            int2(0), int2(int(width) - 1, int(height) - 1));
        storedDepth = shadowTexture.Load(int4(texel, int(layer), 0));
    }
    bool covered = storedDepth < 0.999999;
	bool unbiasedDirect = coordinate.z <= storedDepth;
	bool receiverDirect = true;
	if (mode == ShadowDiagnosticReceiverBiased && IsValidShadowCoordinate(receiverCoordinate))
	{
		int2 receiverTexel = clamp(
			int2(receiverCoordinate.xy * float2(width, height)),
			int2(0), int2(int(width) - 1, int(height) - 1));
		receiverDirect = receiverCoordinate.z
			<= shadowTexture.Load(int4(receiverTexel, int(layer), 0));
	}
	bool finalDirect = true;
	if ((mode == ShadowDiagnosticReceiverNormalOffset || mode == ShadowDiagnosticClassification)
        && IsValidShadowCoordinate(finalCoordinate))
	{
		int2 finalTexel = clamp(
			int2(finalCoordinate.xy * float2(width, height)),
			int2(0), int2(int(width) - 1, int(height) - 1));
		finalDirect = finalCoordinate.z
			<= shadowTexture.Load(int4(finalTexel, int(layer), 0));
	}

    if (mode == ShadowDiagnosticShadowDepthCoverage)
    {
        float depth = saturate(storedDepth);
        result.diagnosticColor = covered
            ? float3(depth * 0.25, depth, depth * 0.25)
            : float3(1.0, 0.0, 0.0);
    }
    else if (mode == ShadowDiagnosticReceiverUnbiased)
		result.diagnosticColor = unbiasedDirect
			? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	else if (mode == ShadowDiagnosticReceiverBiased)
		result.diagnosticColor = receiverDirect
			? float3(0.0, 1.0, 0.25) : float3(1.0, 0.25, 0.0);
	else if (mode == ShadowDiagnosticReceiverNormalOffset)
		result.diagnosticColor = finalDirect
			? float3(0.0, 1.0, 0.5) : float3(1.0, 0.5, 0.0);
    else if (mode == ShadowDiagnosticTexelGrid)
    {
        float2 grid = abs(frac(coordinate.xy * float2(width, height)) - 0.5);
        float2 guardUv = validRegion.xy;
        bool outsideGuard = any(coordinate.xy < guardUv)
            || any(coordinate.xy > 1.0 - guardUv);
        result.diagnosticColor = outsideGuard ? float3(1.0, 1.0, 0.0)
            : (max(grid.x, grid.y) > 0.47
                ? float3(0.0, 1.0, 1.0) : float3(0.05));
    }
    else if (mode == ShadowDiagnosticBiasContributions)
		result.diagnosticColor = totalClamped ? float3(1.0)
			: float3(saturate(rasterBias.w), saturate((texelBias.z - 0.00005) / 0.00015),
				saturate(normalWorld / 0.08));
	else if (mode == ShadowDiagnosticClassification)
	{
		bool displaced = IsValidShadowCoordinate(finalCoordinate)
			&& distance(finalCoordinate, coordinate) > 0.01;
		result.diagnosticColor = !covered ? float3(1.0, 0.0, 0.0)
			: (!unbiasedDirect && finalDirect
				? float3(1.0, 0.65, 0.0)
				: (displaced && totalClamped ? float3(1.0, 0.25, 0.0)
					: (finalDirect
						? float3(1.0) : float3(0.0, 1.0, 0.0))));
	}
    else if (mode == ShadowDiagnosticFilterFootprint)
    {
        // R = bounded tier identity, G = effective footprint radius,
        // B = normalized valid comparison weight at this receiver.
        result.diagnosticColor = float3(
            saturate(filter.z * 0.5),
            saturate(filter.w / 2.5),
            saturate(validTapWeight));
    }
    else if (mode == ShadowDiagnosticFilterTapValidity)
    {
        // Green is fully valid, red is invalid normalized kernel weight.
        result.diagnosticColor = float3(
            saturate(1.0 - validTapWeight),
            saturate(validTapWeight),
            0.0);
    }
    else if (mode == ShadowDiagnosticFilterDifference)
    {
        float unusedLowValidWeight;
        float lowComparison = SampleDirectionalShadowKernel(
            finalCoordinate,
            float4(filter.xy, float(ShadowFilterLow), 0.5),
            validRegion,
            layer,
            shadowTexture,
            shadowSampler,
            unusedLowValidWeight);
        float signedDifference = finalComparison - lowComparison;
        float magnitude = saturate(abs(signedDifference) * 4.0);
        // Red is darker than Low, blue is lighter, green is unchanged.
        result.diagnosticColor = float3(
            signedDifference < 0.0 ? magnitude : 0.0,
            1.0 - magnitude,
            signedDifference > 0.0 ? magnitude : 0.0);
    }
    return result;
}

public FDirectionalShadowSample EvaluateDirectionalShadow(
    float3 worldPosition,
    float3 geometricNormal,
    float4 control,
    float4 viewDepthTransform,
    float4 splitDepths,
    float4 lightTransition,
    FDirectionalShadowCascadeUniform cascades[3],
    Texture2DArray<float> shadowTexture,
    SamplerComparisonState shadowSampler)
{
    FDirectionalShadowSample result;
    result.attenuation = 1.0;
    result.diagnosticColor = float3(1.0);
    // control: enabled, diagnostic mode, cascade count, cascaded-candidate flag.
    uint cascadeCount = min((uint)(control.z + 0.5), 3u);
    bool cascadedCandidate = control.w > 0.5;
    float receiverDepth = dot(viewDepthTransform, float4(worldPosition, 1.0));
    if (control.x < 0.5 || cascadeCount == 0u
        || (cascadedCandidate && (!isfinite(receiverDepth)
            || receiverDepth < splitDepths[0]
            || receiverDepth > splitDepths[cascadeCount])))
        return result;

    uint nearCascade = cascadeCount - 1u;
    [unroll]
    for (uint index = 0u; index < 3u; ++index)
    {
        if (index < cascadeCount && receiverDepth <= splitDepths[index + 1u])
        {
            nearCascade = index;
            break;
        }
    }
    uint farCascade = nearCascade;
    float transitionWeight = 0.0;
    if (nearCascade + 1u < cascadeCount)
    {
        float boundary = splitDepths[nearCascade + 1u];
        float nextBoundary = splitDepths[nearCascade + 2u];
        float transitionStart = boundary
            - lightTransition.w * (nextBoundary - boundary);
        if (receiverDepth >= transitionStart)
        {
            farCascade = nearCascade + 1u;
            transitionWeight = saturate(
                (receiverDepth - transitionStart) / (boundary - transitionStart));
        }
    }

    uint mode = (uint)(control.y + 0.5);
    float4 localControl = control;
    // Cascade diagnostics compose production samples without per-cascade diagnostics.
    if (mode >= ShadowDiagnosticCascadeIndex)
        localControl.y = float(ShadowDiagnosticLit);
    FDirectionalShadowSample nearSample = EvaluateDirectionalShadowCascade(
        worldPosition, geometricNormal, localControl, lightTransition.xyz,
        cascades[nearCascade], nearCascade, shadowTexture, shadowSampler);
    FDirectionalShadowSample farSample = nearSample;
    if (farCascade != nearCascade)
    {
        farSample = EvaluateDirectionalShadowCascade(
            worldPosition, geometricNormal, localControl, lightTransition.xyz,
            cascades[farCascade], farCascade, shadowTexture, shadowSampler);
    }
    result.attenuation = lerp(
        nearSample.attenuation, farSample.attenuation, transitionWeight);
    result.diagnosticColor = lerp(
        nearSample.diagnosticColor, farSample.diagnosticColor, transitionWeight);

    if (mode == ShadowDiagnosticCascadeIndex)
    {
        const float3 cascadeColors[3] = {
            float3(1.0, 0.1, 0.1),
            float3(0.1, 1.0, 0.1),
            float3(0.1, 0.25, 1.0)};
        result.diagnosticColor = lerp(
            cascadeColors[nearCascade], cascadeColors[farCascade],
            transitionWeight);
    }
    else if (mode == ShadowDiagnosticCascadeTransition)
        result.diagnosticColor = float3(transitionWeight);
    else if (mode == ShadowDiagnosticCascadeCoverage)
    {
        float4 coverageControl = control;
        coverageControl.y = float(ShadowDiagnosticShadowDepthCoverage);
        FDirectionalShadowSample nearCoverage = EvaluateDirectionalShadowCascade(
            worldPosition, geometricNormal, coverageControl, lightTransition.xyz,
            cascades[nearCascade], nearCascade, shadowTexture, shadowSampler);
        FDirectionalShadowSample farCoverage = nearCoverage;
        if (farCascade != nearCascade)
        {
            farCoverage = EvaluateDirectionalShadowCascade(
                worldPosition, geometricNormal, coverageControl, lightTransition.xyz,
                cascades[farCascade], farCascade, shadowTexture, shadowSampler);
        }
        result.diagnosticColor = lerp(
            nearCoverage.diagnosticColor, farCoverage.diagnosticColor,
            transitionWeight);
    }
    else if (mode == ShadowDiagnosticCascadeDifference)
    {
        float signedDifference = farSample.attenuation - nearSample.attenuation;
        float magnitude = saturate(abs(signedDifference) * 4.0);
        result.diagnosticColor = float3(
            signedDifference < 0.0 ? magnitude : 0.0,
            1.0 - magnitude,
            signedDifference > 0.0 ? magnitude : 0.0);
    }
    return result;
}
