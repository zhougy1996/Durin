[shader("vertex")]
float4 VertexMain() : SV_Position
{
	return float4(0.0, 0.0, 0.0, 1.0);
}

[shader("fragment")]
float4 FragmentMain() : SV_Target
{
	return float4(0.25, 0.5, 0.75, 1.0);
}
