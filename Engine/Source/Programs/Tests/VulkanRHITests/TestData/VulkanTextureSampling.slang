[[vk::binding(0, 0)]]
Texture2D<float4> LinearTexture;

[[vk::binding(1, 0)]]
Texture2D<float4> SrgbTexture;

[[vk::binding(2, 0)]]
Texture2D<float4> Bc1Texture;

[[vk::binding(3, 0)]]
Texture2D<float4> Bc3Texture;

[[vk::binding(4, 0)]]
Texture2D<float4> Bc5Texture;

[[vk::binding(5, 0)]]
Texture2D<float4> Bc7Texture;

[[vk::binding(6, 0)]]
SamplerState TextureSampler;

[[vk::binding(7, 0)]]
RWStructuredBuffer<float4> Results;

[numthreads(1, 1, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const float2 uv = float2(0.5, 0.5);
	Results[0] = LinearTexture.SampleLevel(TextureSampler, uv, 0.0);
	Results[1] = LinearTexture.SampleLevel(TextureSampler, uv, 1.0);
	Results[2] = LinearTexture.SampleLevel(TextureSampler, uv, 2.0);
	Results[3] = SrgbTexture.SampleLevel(TextureSampler, uv, 0.0);
	Results[4] = SrgbTexture.SampleLevel(TextureSampler, uv, 1.0);
	Results[5] = SrgbTexture.SampleLevel(TextureSampler, uv, 2.0);
	Results[6] = Bc1Texture.SampleLevel(TextureSampler, uv, 0.0);
	Results[7] = Bc1Texture.SampleLevel(TextureSampler, uv, 1.0);
	Results[8] = Bc1Texture.SampleLevel(TextureSampler, uv, 2.0);
	Results[9] = Bc3Texture.SampleLevel(TextureSampler, uv, 0.0);
	Results[10] = Bc3Texture.SampleLevel(TextureSampler, uv, 1.0);
	Results[11] = Bc3Texture.SampleLevel(TextureSampler, uv, 2.0);
	Results[12] = Bc5Texture.SampleLevel(TextureSampler, uv, 0.0);
	Results[13] = Bc5Texture.SampleLevel(TextureSampler, uv, 1.0);
	Results[14] = Bc5Texture.SampleLevel(TextureSampler, uv, 2.0);
	Results[15] = Bc7Texture.SampleLevel(TextureSampler, uv, 0.0);
	Results[16] = Bc7Texture.SampleLevel(TextureSampler, uv, 1.0);
	Results[17] = Bc7Texture.SampleLevel(TextureSampler, uv, 2.0);
}
