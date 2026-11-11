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
    float4 planes[6];
    float4 worldOriginAndRelativeTolerance;
    uint4 counts; // candidates, planes, padding
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
    if (index >= Params.counts.x)
        return;

    const Candidate candidate = Candidates[index];
    // Only tolerance uses world magnitudes. Plane distances and bounds stay camera-relative.
    const float3 origin = Params.worldOriginAndRelativeTolerance.xyz;
    const float3 magnitude = max(abs(origin + candidate.boundsMin), abs(origin + candidate.boundsMax));
    const float tolerance = Params.worldOriginAndRelativeTolerance.w * max(1.0, max(magnitude.x, max(magnitude.y, magnitude.z)));
    for (uint i = 0; i < Params.counts.y; ++i)
    {
        const float4 plane = Params.planes[i];
        const float3 support = select(plane.xyz >= 0.0, candidate.boundsMax, candidate.boundsMin);
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
    VisibleInstances[firstInstance + visibleSlot] = candidate.transformIndex;
}
