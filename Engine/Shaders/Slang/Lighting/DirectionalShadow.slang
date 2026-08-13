module DirectionalShadow;

public struct FDirectionalShadowSample
{
    public float attenuation;
    public float3 diagnosticColor;
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

public FDirectionalShadowSample EvaluateDirectionalShadow(
    float3 worldPosition,
    float3 worldNormal,
    float4x4 worldToShadow,
    float4 control,
    float4 texelBias,
    float4 rasterBias,
    float4 lightBounds,
    Texture2D<float> shadowTexture,
    SamplerComparisonState shadowSampler)
{
    FDirectionalShadowSample result;
    result.attenuation = 1.0;
    result.diagnosticColor = float3(1.0);
    if (control.x < 0.5)
        return result;
    float3 coordinate = ProjectShadow(worldPosition, worldToShadow);
    if (!IsValidShadowCoordinate(coordinate))
    {
        result.diagnosticColor = float3(0.1, 0.25, 1.0);
        return result;
    }
    uint width;
    uint height;
    shadowTexture.GetDimensions(width, height);
    int2 texel = clamp(int2(coordinate.xy * float2(width, height)),
        int2(0), int2(int(width) - 1, int(height) - 1));
    float storedDepth = shadowTexture.Load(int3(texel, 0));
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
	float finalComparison = IsValidShadowCoordinate(finalCoordinate)
		? shadowTexture.SampleCmpLevelZero(
			shadowSampler, finalCoordinate.xy, finalCoordinate.z)
		: 1.0;
	result.attenuation = finalComparison;

	bool unbiasedDirect = coordinate.z <= storedDepth;
	bool receiverDirect = true;
	if (IsValidShadowCoordinate(receiverCoordinate))
	{
		int2 receiverTexel = clamp(
			int2(receiverCoordinate.xy * float2(width, height)),
			int2(0), int2(int(width) - 1, int(height) - 1));
		receiverDirect = receiverCoordinate.z
			<= shadowTexture.Load(int3(receiverTexel, 0));
	}
	bool finalDirect = true;
	if (IsValidShadowCoordinate(finalCoordinate))
	{
		int2 finalTexel = clamp(
			int2(finalCoordinate.xy * float2(width, height)),
			int2(0), int2(int(width) - 1, int(height) - 1));
		finalDirect = finalCoordinate.z
			<= shadowTexture.Load(int3(finalTexel, 0));
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
    return result;
}
