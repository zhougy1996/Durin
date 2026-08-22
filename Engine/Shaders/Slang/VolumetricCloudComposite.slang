module VolumetricCloudComposite;

struct VSInput { float2 pos : POSITION; float2 uv : TEXCOORD; };
struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };

[[vk::binding(0, 0)]] Texture2D<float4> SceneColorTexture;
[[vk::binding(1, 0)]] Texture2D<float4> CloudTexture;
[[vk::binding(2, 0)]] Texture2D<float> SceneDepthTexture;
[[vk::binding(3, 0)]] Texture2D<float> DebugTexture;

struct VolumetricCloudCompositeUniform
{
	float4 Extent; // output.xy, cloud.zw
	float4 Viewport; // full-resolution origin.xy and extent.zw
	float4 Debug; // mode.x, temporal status.y (-1 unavailable), shadow available.z
};
[[vk::binding(4, 0)]] ConstantBuffer<VolumetricCloudCompositeUniform> Params;

[shader("vertex")]
VSOutput VertexMain(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    return output;
}

// Ping-pong composition preserves scene alpha and applies the frozen
// radiance/transmittance algebra in linear space.
[shader("fragment")]
float4 FragmentMain(VSOutput input) : SV_Target0
{
	int3 pixel = int3(int2(input.pos.xy), 0);
	float4 scene = SceneColorTexture.Load(pixel);
	int2 outputExtent = int2(Params.Extent.xy);
	int2 cloudExtent = int2(Params.Extent.zw);
	if (any(pixel.xy < int2(Params.Viewport.xy))
		|| any(pixel.xy >= int2(Params.Viewport.xy + Params.Viewport.zw)))
		return scene;
	float4 cloud;
	if (all(outputExtent == cloudExtent))
	{
		cloud = CloudTexture.Load(pixel);
	}
	else
	{
		float2 cloudPosition = (float2(pixel.xy) + 0.5)
			* Params.Extent.zw / Params.Extent.xy - 0.5;
		int2 base = int2(floor(cloudPosition));
		float2 fraction = frac(cloudPosition);
		float centerDepth = SceneDepthTexture.Load(pixel);
		float4 accumulated = 0.0;
		float totalWeight = 0.0;
		float4 nearestCloud = CloudTexture.Load(int3(
			clamp(int2(round(cloudPosition)), int2(0), cloudExtent - 1), 0));
		for (int y = 0; y < 2; ++y)
		{
			for (int x = 0; x < 2; ++x)
			{
				int2 tap = clamp(base + int2(x, y), int2(0), cloudExtent - 1);
				float2 axisWeight = 1.0 - abs(float2(x, y) - fraction);
				float spatialWeight = axisWeight.x * axisWeight.y;
				int2 sourcePixel = clamp(int2(floor(
					(float2(tap) + 0.5) * Params.Extent.xy / Params.Extent.zw)),
					int2(0), outputExtent - 1);
				float tapDepth = SceneDepthTexture.Load(int3(sourcePixel, 0));
				// A hard device-depth agreement gate is both cheaper and safer at
				// opaque silhouettes than a broad exponential tail. When no tap
				// agrees, the frozen nearest-cloud fallback below remains active.
				float depthWeight = abs(tapDepth - centerDepth) <= 1.0e-4 ? 1.0 : 0.0;
				float weight = spatialWeight * depthWeight;
				accumulated += CloudTexture.Load(int3(tap, 0)) * weight;
				totalWeight += weight;
			}
		}
		cloud = totalWeight > 1.0e-5 ? accumulated / totalWeight : nearestCloud;
	}
	int debugMode = int(Params.Debug.x + 0.5);
	if (debugMode == 1)
		return float4(cloud.rgb, 1.0);
	if (debugMode == 2)
		return float4(cloud.aaa, 1.0);
	if (debugMode == 3)
	{
		if (Params.Debug.y < -0.5) return float4(0.35, 0.35, 0.35, 1.0);
		return Params.Debug.y > 0.5
			? float4(0.15, 0.85, 0.25, 1.0)
			: float4(0.95, 0.58, 0.12, 1.0);
	}
	if (debugMode == 4)
	{
		if (Params.Debug.z < 0.5) return float4(0.35, 0.35, 0.35, 1.0);
		float visibility = DebugTexture.Load(pixel);
		return float4(visibility.xxx, 1.0);
	}
	return float4(cloud.rgb + cloud.a * scene.rgb, scene.a);
}
