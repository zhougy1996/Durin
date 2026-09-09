[shader("vertex")]
float4 VertexMain(uint Index : SV_VertexID) : SV_Position
{
	return float4(float(Index & 1) * 2.0 - 1.0, float(Index >> 1) * 2.0 - 1.0, 0.0, 1.0);
}
[shader("fragment")]
float4 FragmentMain() : SV_Target { return float4(0.25, 0.5, 0.75, 1.0); }
[shader("compute")]
[numthreads(1, 1, 1)]
void ComputeMain(uint3 Index : SV_DispatchThreadID) {}
