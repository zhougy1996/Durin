[[vk::binding(0, 0)]]
Texture3D<float4> InputVolume;

[[vk::binding(1, 0)]]
SamplerState VolumeSampler;

[[vk::binding(2, 0)]]
RWTexture3D<float4> StorageVolume;

[[vk::binding(3, 0)]]
RWTexture2D<float4> OutputImage;

[numthreads(1, 1, 1)]
void ComputeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
	const uint index = dispatchThreadId.x;
	const float z = (float(index) + 0.5) / 2.0;
	const float4 value = InputVolume.SampleLevel(
		VolumeSampler, float3(0.5, 0.5, z), 0.0);
	StorageVolume[int3(index, 0, 0)] = value;
	OutputImage[int2(index, 0)] = value;
}
