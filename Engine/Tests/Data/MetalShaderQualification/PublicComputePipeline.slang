[[vk::binding(0, 0)]]
RWStructuredBuffer<uint> OutputBuffer;

[[vk::binding(1, 0)]]
RWTexture2D<float4> OutputImage;

struct FPushConstants
{
	uint Increment;
};

[[vk::push_constant]]
ConstantBuffer<FPushConstants> PushConstants;

[numthreads(1, 1, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	const uint red = 16 + index + PushConstants.Increment;
	const uint green = 32 + index + PushConstants.Increment;
	const uint blue = 48 + index + PushConstants.Increment;
	OutputBuffer[index] = red | (green << 8) | (blue << 16) | (255 << 24);
	OutputImage[int2(index, 0)] = float4(
		float(red) / 255.0, float(green) / 255.0,
		float(blue) / 255.0, 1.0);
}

[numthreads(1, 1, 1)]
void ComputeSecondMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	const uint previous = OutputBuffer[index];
	const uint red = (previous & 255) + PushConstants.Increment;
	const uint green = ((previous >> 8) & 255) + PushConstants.Increment;
	const uint blue = ((previous >> 16) & 255) + PushConstants.Increment;
	OutputBuffer[index] = red | (green << 8) | (blue << 16) | (255 << 24);
	OutputImage[int2(index, 0)] = float4(
		float(red) / 255.0, float(green) / 255.0,
		float(blue) / 255.0, 1.0);
}
