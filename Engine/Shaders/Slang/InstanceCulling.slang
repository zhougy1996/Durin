module InstanceCulling;

struct Candidate
{
    uint instanceId;
    uint argumentIndex;
};

struct CullingUniform
{
    float4 planes[6];
    float4 worldOriginAndRelativeTolerance;
    float4 viewOriginLow;
    uint4 counts; // candidates, planes, padding, padding
};

[[vk::binding(0, 0)]] StructuredBuffer<Candidate> Candidates;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> VisibleInstances;
[[vk::binding(2, 0)]] RWByteAddressBuffer IndirectArguments;
[[vk::binding(3, 0)]] ConstantBuffer<CullingUniform> Params;
[[vk::binding(4, 0)]] StructuredBuffer<float4> InstanceSceneData;

float4 ReadInstanceData(Candidate candidate, uint offset)
{
    uint index = candidate.instanceId * 18 + offset;
    return InstanceSceneData[index];
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CullMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index >= Params.counts.x)
        return;

    const Candidate candidate = Candidates[index];
    const float3 instanceHigh = ReadInstanceData(candidate, 12).xyz;
    const float3 instanceLow = ReadInstanceData(candidate, 13).xyz;
    const float4 packedMin = ReadInstanceData(candidate, 14);
    const float3 packedMax = ReadInstanceData(candidate, 15).xyz;
    const float3 relativeOrigin = (instanceHigh - Params.worldOriginAndRelativeTolerance.xyz)
        + (instanceLow - Params.viewOriginLow.xyz);
    const float3 boundsMin = relativeOrigin + packedMin.xyz;
    const float3 boundsMax = relativeOrigin + packedMax;
    // Only tolerance uses world magnitudes. Plane distances and bounds stay camera-relative.
    const float3 origin = Params.worldOriginAndRelativeTolerance.xyz;
    const float3 magnitude = max(abs(origin + boundsMin), abs(origin + boundsMax));
    const float tolerance = Params.worldOriginAndRelativeTolerance.w * max(1.0, max(magnitude.x, max(magnitude.y, magnitude.z)));
    for (uint i = 0; i < (packedMin.w != 0.0 ? Params.counts.y : 0); ++i)
    {
        const float4 plane = Params.planes[i];
        const float3 support = select(plane.xyz >= 0.0, boundsMax, boundsMin);
        // Retain borderline boxes when the packed plane or dot product rounds.
        const float roundoff = 8.0 * 1.1920929e-7 * (dot(abs(plane.xyz), abs(support)) + abs(plane.w));
        if (dot(plane.xyz, support) + plane.w < -(tolerance + roundoff))
            return;
    }

    uint visibleSlot = 0;
    IndirectArguments.InterlockedAdd(
        candidate.argumentIndex * 20 + 4, 1, visibleSlot);
    const uint firstInstance =
        IndirectArguments.Load(candidate.argumentIndex * 20 + 16);
    VisibleInstances[firstInstance + visibleSlot] = candidate.instanceId;
}
