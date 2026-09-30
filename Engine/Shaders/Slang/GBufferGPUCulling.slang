module GBufferGPUCulling;

struct Candidate
{
    float3 boundsMin;
    uint argumentIndex;
    float3 boundsMax;
    uint transformIndex;
};

struct CullingUniform
{
    float4x4 viewProjection;
    uint candidateCount;
    uint3 padding;
};

[[vk::binding(0, 0)]] StructuredBuffer<Candidate> Candidates;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> VisibleInstances;
[[vk::binding(2, 0)]] RWByteAddressBuffer IndirectArguments;
[[vk::binding(3, 0)]] ConstantBuffer<CullingUniform> Params;

[shader("compute")]
[numthreads(64, 1, 1)]
void CullMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= Params.candidateCount)
        return;

    const Candidate candidate = Candidates[index];
    const float3 boundsMin = candidate.boundsMin;
    const float3 boundsMax = candidate.boundsMax;
    const float3 center3 = (boundsMin + boundsMax) * 0.5;
    const float3 extent = (boundsMax - boundsMin) * 0.5;
    const float4 center = float4(center3, 1.0);
    const float4 leftPlane = Params.viewProjection[3]
        + Params.viewProjection[0];
    const float4 rightPlane = Params.viewProjection[3]
        - Params.viewProjection[0];
    const float4 bottomPlane = Params.viewProjection[3]
        + Params.viewProjection[1];
    const float4 topPlane = Params.viewProjection[3]
        - Params.viewProjection[1];
    const float4 nearPlane = Params.viewProjection[2];
    const float4 farPlane = Params.viewProjection[3]
        - Params.viewProjection[2];
    const bool visible = dot(leftPlane, center)
            >= -dot(abs(leftPlane.xyz), extent)
        && dot(rightPlane, center) >= -dot(abs(rightPlane.xyz), extent)
        && dot(bottomPlane, center) >= -dot(abs(bottomPlane.xyz), extent)
        && dot(topPlane, center) >= -dot(abs(topPlane.xyz), extent)
        && dot(nearPlane, center) >= -dot(abs(nearPlane.xyz), extent)
        && dot(farPlane, center) >= -dot(abs(farPlane.xyz), extent);
    if (!visible)
        return;

    uint visibleSlot = 0;
    IndirectArguments.InterlockedAdd(
        candidate.argumentIndex * 20 + 4, 1, visibleSlot);
    const uint firstInstance =
        IndirectArguments.Load(candidate.argumentIndex * 20 + 16);
    VisibleInstances[firstInstance + visibleSlot] = candidate.transformIndex;
}
