[shader("vertex")]
float4 VertexMain(uint VertexIndex : SV_VertexID) : SV_Position
{
	static const float2 Positions[3] = {
		float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
	return float4(Positions[VertexIndex], 0.0, 1.0);
}

struct FragmentOutput
{
	float4 Color0 : SV_Target0;
	float4 Color1 : SV_Target1;
};

[shader("fragment")]
float4 FragmentMain() : SV_Target
{
	return float4(0.25, 0.5, 0.75, 1.0);
}

[shader("fragment")]
FragmentOutput FragmentMrtMain()
{
	FragmentOutput Output;
	Output.Color0 = float4(0.25, 0.5, 0.75, 1.0);
	Output.Color1 = Output.Color0;
	return Output;
}
