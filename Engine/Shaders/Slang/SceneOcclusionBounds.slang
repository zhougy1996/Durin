module SceneOcclusionBounds;

struct ProjectedBounds
{
    uint FirstX, FirstY, LastX, LastY;
    uint Stride, Reversed;
    float Nearest;
};
StructuredBuffer<ProjectedBounds> TestBounds;
StructuredBuffer<float2> TestDepthTiles;
RWStructuredBuffer<uint> Occluded;
groupshared uint AnyVisible;

[shader("compute")]
[numthreads(32, 1, 1)]
void TestBoundsMain(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ProjectedBounds b = TestBounds[group.x];
    if (b.Stride == 0)
    {
        if (lane == 0) Occluded[group.x] = 0;
        return;
    }
    if (lane == 0) AnyVisible = 0;
    GroupMemoryBarrierWithGroupSync();
    uint width = b.LastX - b.FirstX + 1;
    uint count = width * (b.LastY - b.FirstY + 1);
    for (uint i = lane; i < count; i += 32)
    {
        float2 z = TestDepthTiles[(b.FirstY + i / width) * b.Stride + b.FirstX + i % width];
        bool hidden = b.Reversed != 0 ? b.Nearest < z.x - 0.0001f : b.Nearest > z.y + 0.0001f;
        if (!hidden)
        {
            InterlockedOr(AnyVisible, 1);
            break;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0) Occluded[group.x] = AnyVisible == 0 ? 1 : 0;
}
