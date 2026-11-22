module SceneOcclusion;

Texture2D<float> SceneDepth;
RWStructuredBuffer<float2> DepthTiles;
groupshared uint MinDepth;
groupshared uint MaxDepth;

[shader("compute")]
[numthreads(8, 8, 1)]
void ReduceDepth(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    uint width, height;
    SceneDepth.GetDimensions(width, height);
    if (lane == 0) { MinDepth = asuint(1.0f); MaxDepth = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint2 first = group.xy * 16 + thread.xy * 2;
    for (uint y = 0; y < 2; ++y)
        for (uint x = 0; x < 2; ++x)
        {
            uint2 p = first + uint2(x, y);
            if (p.x >= width || p.y >= height) continue;
            float z = SceneDepth.Load(int3(p, 0));
            bool valid = isfinite(z) && z >= 0 && z <= 1;
            InterlockedMin(MinDepth, asuint(valid ? max(z, 0.0f) : 0.0f));
            InterlockedMax(MaxDepth, asuint(valid ? max(z, 0.0f) : 1.0f));
        }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
        DepthTiles[group.y * ((width + 15) / 16) + group.x] = float2(asfloat(MinDepth), asfloat(MaxDepth));
}
