[[vk::binding(0, 0)]] RWTexture2D<float4> StorageImage;
[[vk::binding(0, 0)]] Texture2D<float4> SampledImage;
[[vk::binding(1, 0)]] SamplerState SampledImageSampler;

[shader("vertex")]
float4 VertexMain(uint VertexIndex : SV_VertexID) : SV_Position
{
	static const float2 Positions[3] = {
		float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
	return float4(Positions[VertexIndex], 0.0, 1.0);
}

[shader("fragment")]
float4 StorageMain(float4 Position : SV_Position) : SV_Target
{
	const float4 Value = float4(0.25, 0.5, 0.75, 1.0);
	StorageImage[int2(Position.xy)] = Value;
	return Value;
}

[shader("fragment")]
float4 SampleMain(float4 Position : SV_Position) : SV_Target
{
	return SampledImage.SampleLevel(SampledImageSampler, float2(0.5, 0.5), 0.0);
}
