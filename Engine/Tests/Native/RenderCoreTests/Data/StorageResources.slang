StructuredBuffer<float4> ReadBuffer;
RWStructuredBuffer<float4> WriteBuffer;
RWTexture2D<float4> OutputImage;

[shader("fragment")]
float4 fragmentMain(float4 Position : SV_Position) : SV_Target
{
	float4 Value = ReadBuffer[0];
	WriteBuffer[0] = Value;
	OutputImage[int2(Position.xy)] = Value;
	return Value;
}
