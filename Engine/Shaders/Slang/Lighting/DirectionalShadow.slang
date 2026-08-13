module DirectionalShadow;

public struct FDirectionalShadowSample
{
    public float attenuation;
    public float3 diagnosticColor;
};

public struct FDirectionalShadowCascadeUniform
{
    public float4x4 WorldToShadow;
    public float4 TexelBias;
    public float4 RasterBias;
    public float4 Filter;
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
    uint quality = min((uint)(filter.z + 0.5), 2u);
    int radius = quality == 0u ? 0 : (quality == 1u ? 1 : 2);
    float normalization = quality == 0u ? 1.0
        : (quality == 1u ? 1.0 / 16.0 : 1.0 / 81.0);
    float accumulated = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            if (abs(x) > radius || abs(y) > radius)
                continue;
            int axisWeightX = quality == 0u ? 1
                : (quality == 1u ? (x == 0 ? 2 : 1) : (3 - abs(x)));
            int axisWeightY = quality == 0u ? 1
                : (quality == 1u ? (y == 0 ? 2 : 1) : (3 - abs(y)));
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
    float3 worldNormal,
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
    float4 lightBounds = float4(lightDirection, validRegion.x * 2048.0);
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
    int2 texel = clamp(int2(coordinate.xy * float2(width, height)),
        int2(0), int2(int(width) - 1, int(height) - 1));
    float storedDepth = shadowTexture.Load(int4(texel, int(layer), 0));
    bool covered = storedDepth < 0.999999;
	float normalLengthSquared = dot(worldNormal, worldNormal);
	float lightLengthSquared = dot(lightBounds.xyz, lightBounds.xyz);
	float texelWorld = max(texelBias.x, texelBias.y);
	bool biasInputsValid = control.z < 0.5
		&& all(isfinite(worldNormal)) && normalLengthSquared > 1.0e-8
		&& all(isfinite(lightBounds.xyz)) && lightLengthSquared > 1.0e-8
		&& all(isfinite(texelBias.xy)) && texelWorld > 0.0;
	float3 normal = biasInputsValid
		? worldNormal * rsqrt(normalLengthSquared) : float3(0.0, 0.0, 1.0);
	float3 toLight = biasInputsValid
		? -lightBounds.xyz * rsqrt(lightLengthSquared) : float3(0.0, 0.0, 1.0);
	float grazing = biasInputsValid
		? 1.0 - saturate(abs(dot(normal, toLight))) : 0.0;
	float receiverWorld = biasInputsValid
		? clamp(texelWorld * (0.05 + 0.10 * grazing), 0.0005, 0.02) : 0.0;
	float normalWorld = biasInputsValid
		? clamp(texelWorld * (0.20 + 0.55 * grazing), 0.0, 0.10) : 0.0;
	float maximumTotal = min(0.75 * texelWorld, 0.10);
	bool totalClamped = biasInputsValid
		&& receiverWorld + normalWorld > maximumTotal;
	if (totalClamped)
		normalWorld = max(0.0, maximumTotal - receiverWorld);
	float3 receiverCoordinate = biasInputsValid
		? ProjectShadow(worldPosition + toLight * receiverWorld, worldToShadow)
		: coordinate - float3(0.0, 0.0, 0.0005);
	float3 finalCoordinate = biasInputsValid
		? ProjectShadow(worldPosition + toLight * receiverWorld
			+ normal * normalWorld, worldToShadow)
		: receiverCoordinate;
	float validTapWeight = 0.0;
	float finalComparison = SampleDirectionalShadowKernel(
		finalCoordinate, filter, validRegion, layer,
		shadowTexture, shadowSampler, validTapWeight);
	result.attenuation = finalComparison;

	bool unbiasedDirect = coordinate.z <= storedDepth;
	bool receiverDirect = true;
	if (IsValidShadowCoordinate(receiverCoordinate))
	{
		int2 receiverTexel = clamp(
			int2(receiverCoordinate.xy * float2(width, height)),
			int2(0), int2(int(width) - 1, int(height) - 1));
		receiverDirect = receiverCoordinate.z
			<= shadowTexture.Load(int4(receiverTexel, int(layer), 0));
	}
	bool finalDirect = true;
	if (IsValidShadowCoordinate(finalCoordinate))
	{
		int2 finalTexel = clamp(
			int2(finalCoordinate.xy * float2(width, height)),
			int2(0), int2(int(width) - 1, int(height) - 1));
		finalDirect = finalCoordinate.z
			<= shadowTexture.Load(int4(finalTexel, int(layer), 0));
	}

    uint mode = (uint)(control.y + 0.5);
    if (mode == 1u)
    {
        float depth = saturate(storedDepth);
        result.diagnosticColor = covered
            ? float3(depth * 0.25, depth, depth * 0.25)
            : float3(1.0, 0.0, 0.0);
    }
    else if (mode == 2u)
		result.diagnosticColor = unbiasedDirect
			? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
	else if (mode == 3u)
		result.diagnosticColor = receiverDirect
			? float3(0.0, 1.0, 0.25) : float3(1.0, 0.25, 0.0);
	else if (mode == 4u)
		result.diagnosticColor = finalDirect
			? float3(0.0, 1.0, 0.5) : float3(1.0, 0.5, 0.0);
    else if (mode == 5u)
    {
        float2 grid = abs(frac(coordinate.xy * float2(width, height)) - 0.5);
        float2 guardUv = lightBounds.ww / float2(width, height);
        bool outsideGuard = any(coordinate.xy < guardUv)
            || any(coordinate.xy > 1.0 - guardUv);
        result.diagnosticColor = outsideGuard ? float3(1.0, 1.0, 0.0)
            : (max(grid.x, grid.y) > 0.47
                ? float3(0.0, 1.0, 1.0) : float3(0.05));
    }
    else if (mode == 6u)
		result.diagnosticColor = totalClamped ? float3(1.0)
			: float3(saturate(rasterBias.w),
				saturate(receiverWorld / 0.02), saturate(normalWorld / 0.10));
	else if (mode == 7u)
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
    else if (mode == 8u)
    {
        // R = bounded tier identity, G = effective footprint radius,
        // B = normalized valid comparison weight at this receiver.
        result.diagnosticColor = float3(
            saturate(filter.z * 0.5),
            saturate(filter.w / 2.5),
            saturate(validTapWeight));
    }
    else if (mode == 9u)
    {
        // Green is fully valid, red is invalid normalized kernel weight.
        result.diagnosticColor = float3(
            saturate(1.0 - validTapWeight),
            saturate(validTapWeight),
            0.0);
    }
    else if (mode == 10u)
    {
        float unusedLowValidWeight;
        float lowComparison = SampleDirectionalShadowKernel(
            finalCoordinate,
            float4(filter.xy, 0.0, 0.5),
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
    float3 worldNormal,
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
    if (mode >= 11u)
        localControl.y = 0.0;
    FDirectionalShadowSample nearSample = EvaluateDirectionalShadowCascade(
        worldPosition, worldNormal, localControl, lightTransition.xyz,
        cascades[nearCascade], nearCascade, shadowTexture, shadowSampler);
    FDirectionalShadowSample farSample = nearSample;
    if (farCascade != nearCascade)
    {
        farSample = EvaluateDirectionalShadowCascade(
            worldPosition, worldNormal, localControl, lightTransition.xyz,
            cascades[farCascade], farCascade, shadowTexture, shadowSampler);
    }
    result.attenuation = lerp(
        nearSample.attenuation, farSample.attenuation, transitionWeight);
    result.diagnosticColor = lerp(
        nearSample.diagnosticColor, farSample.diagnosticColor, transitionWeight);

    if (mode == 11u)
    {
        const float3 cascadeColors[3] = {
            float3(1.0, 0.1, 0.1),
            float3(0.1, 1.0, 0.1),
            float3(0.1, 0.25, 1.0)};
        result.diagnosticColor = lerp(
            cascadeColors[nearCascade], cascadeColors[farCascade],
            transitionWeight);
    }
    else if (mode == 12u)
        result.diagnosticColor = float3(transitionWeight);
    else if (mode == 13u)
    {
        float4 coverageControl = control;
        coverageControl.y = 1.0;
        FDirectionalShadowSample nearCoverage = EvaluateDirectionalShadowCascade(
            worldPosition, worldNormal, coverageControl, lightTransition.xyz,
            cascades[nearCascade], nearCascade, shadowTexture, shadowSampler);
        FDirectionalShadowSample farCoverage = nearCoverage;
        if (farCascade != nearCascade)
        {
            farCoverage = EvaluateDirectionalShadowCascade(
                worldPosition, worldNormal, coverageControl, lightTransition.xyz,
                cascades[farCascade], farCascade, shadowTexture, shadowSampler);
        }
        result.diagnosticColor = lerp(
            nearCoverage.diagnosticColor, farCoverage.diagnosticColor,
            transitionWeight);
    }
    else if (mode == 14u)
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
