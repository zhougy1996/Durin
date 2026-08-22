module VolumetricCloudTemporal;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

struct VolumetricCloudTemporalUniform
{
	float4x4 InverseViewProjection;
	float4x4 PreviousViewProjection;
	float4 Layer;
	float4 CameraPosition;
	float4 Viewport; // cloud-target origin.xy and extent.zw
	float4 Target;   // target extent.xy, history weight.z, history valid.w
};

[[vk::binding(0, 0)]] Texture2D<float4> CurrentCloud;
[[vk::binding(1, 0)]] Texture2D<float4> PreviousCloud;
[[vk::binding(2, 0)]] SamplerState HistorySampler;
[[vk::binding(3, 0)]] ConstantBuffer<VolumetricCloudTemporalUniform> Params;

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

bool IntersectLayer(float3 origin, float3 direction,
	out float nearDistance, out float farDistance)
{
	nearDistance = 0.0;
	farDistance = Params.Layer.z;
	if (abs(direction.z) <= 1.0e-7)
		return origin.z >= Params.Layer.x && origin.z <= Params.Layer.y;
	float first = (Params.Layer.x - origin.z) / direction.z;
	float second = (Params.Layer.y - origin.z) / direction.z;
	nearDistance = max(0.0, min(first, second));
	farDistance = min(Params.Layer.z, max(first, second));
	return farDistance > nearDistance;
}

[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target0
{
	int2 pixel = int2(input.pos.xy);
	float4 current = CurrentCloud.Load(int3(pixel, 0));
	if (Params.Target.w < 0.5)
		return current;

	float2 localUV = (float2(pixel) + 0.5 - Params.Viewport.xy)
		/ Params.Viewport.zw;
	float nearDeviceDepth = Params.Layer.w > 0.5 ? 1.0 : 0.0;
	float farDeviceDepth = 1.0 - nearDeviceDepth;
	float3 nearWorld = ReconstructWorld(localUV, nearDeviceDepth);
	float3 farWorld = ReconstructWorld(localUV, farDeviceDepth);
	float3 direction = normalize(farWorld - nearWorld);
	float nearDistance;
	float farDistance;
	if (!IntersectLayer(Params.CameraPosition.xyz, direction,
		nearDistance, farDistance))
		return current;
	float3 representative = Params.CameraPosition.xyz + direction
		* (nearDistance + farDistance) * 0.5;
	float4 previousClip = mul(Params.PreviousViewProjection,
		float4(representative, 1.0));
	if (previousClip.w <= 1.0e-6)
		return current;
	float2 previousUV = previousClip.xy / previousClip.w * 0.5 + 0.5;
	if (any(previousUV < 0.0) || any(previousUV > 1.0))
		return current;
	float2 historyPixel = Params.Viewport.xy
		+ previousUV * Params.Viewport.zw;
	float2 historyUV = (historyPixel + 0.5) / Params.Target.xy;
	float4 history = PreviousCloud.SampleLevel(HistorySampler, historyUV, 0.0);

	float4 neighborhoodMin = current;
	float4 neighborhoodMax = current;
	int2 extent = int2(Params.Target.xy);
	static const int2 offsets[4] =
	{
		int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
	};
	for (int index = 0; index < 4; ++index)
	{
		float4 sampleValue = CurrentCloud.Load(int3(
			clamp(pixel + offsets[index], int2(0), extent - 1), 0));
		neighborhoodMin = min(neighborhoodMin, sampleValue);
		neighborhoodMax = max(neighborhoodMax, sampleValue);
	}
	history = clamp(history, neighborhoodMin, neighborhoodMax);
	return lerp(current, history, saturate(Params.Target.z));
}
