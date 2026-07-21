[[vk::binding(0, 0)]] StructuredBuffer<float4> ReadBuffer;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> WriteBuffer;
[[vk::binding(2, 0)]] RWTexture2D<float4> OutputImage;

[shader("fragment")]
float4 fragmentMain(float4 Position : SV_Position) : SV_Target
{
	float4 Value = ReadBuffer[0];
	WriteBuffer[0] = Value;
	OutputImage[int2(Position.xy)] = Value;
	return Value;
}
