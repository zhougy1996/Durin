[[vk::binding(0, 0)]]
Texture2D<float4> ComputeOutput;

[shader("vertex")]
float4 VertexMain(uint vertexIndex : SV_VertexID) : SV_Position
{
	static const float2 positions[3] = {
		float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
	return float4(positions[vertexIndex], 0.0, 1.0);
}

[shader("fragment")]
float4 FragmentMain(float4 position : SV_Position) : SV_Target
{
	return ComputeOutput.Load(int3(int2(position.xy), 0));
}
