module SceneData;
import Material.MeshUniforms;

[[vk::binding(27, 0)]] public StructuredBuffer<float4> GPUScenePrimitiveSceneData;
[[vk::binding(28, 0)]] public StructuredBuffer<float4> GPUSceneInstanceSceneData;
[[vk::binding(29, 0)]] public StructuredBuffer<float4> GPUSceneInstancePayloadData;
[[vk::binding(31, 0)]] public StructuredBuffer<uint> GPUSceneInstanceIds;

public struct FInstanceSceneData
{
    public uint InstanceId;
    public uint PrimitiveId;
    public uint InstanceRelativeId;
    public uint InstancePayloadDataOffset;
};

public FInstanceSceneData GetInstanceSceneData(uint instanceId)
{
    uint4 header = asuint(GPUSceneInstanceSceneData[instanceId * 18 + 16]);
    FInstanceSceneData result;
    result.InstanceId = instanceId;
    result.PrimitiveId = header.x;
    result.InstanceRelativeId = header.y;
    result.InstancePayloadDataOffset = header.w;
    return result;
}

public MaterialPrimitiveUniform GetPrimitiveData(uint primitiveId)
{
    MaterialPrimitiveUniform result = (MaterialPrimitiveUniform)0;
    uint first = primitiveId * 25;
    result.LocalToWorld = float4x4(GPUScenePrimitiveSceneData[first], GPUScenePrimitiveSceneData[first + 1], GPUScenePrimitiveSceneData[first + 2], GPUScenePrimitiveSceneData[first + 3]);
    result.WorldToLocal = float4x4(GPUScenePrimitiveSceneData[first + 4], GPUScenePrimitiveSceneData[first + 5], GPUScenePrimitiveSceneData[first + 6], GPUScenePrimitiveSceneData[first + 7]);
    result.NormalToWorld = float4x4(GPUScenePrimitiveSceneData[first + 8], GPUScenePrimitiveSceneData[first + 9], GPUScenePrimitiveSceneData[first + 10], GPUScenePrimitiveSceneData[first + 11]);
    result.WorldOriginHigh = GPUScenePrimitiveSceneData[first + 12];
    result.WorldOriginLow = GPUScenePrimitiveSceneData[first + 13];
    result.BoundsCenter = float4((GPUScenePrimitiveSceneData[first + 14].xyz + GPUScenePrimitiveSceneData[first + 15].xyz) * 0.5, 1.0);
    result.TransformParams = float4(GPUScenePrimitiveSceneData[first + 15].w, 0.0, 0.0, 0.0);
    for (uint slot = 0; slot < 8; ++slot)
        result.CustomPrimitiveData[slot] = GPUScenePrimitiveSceneData[primitiveId * 25 + 17 + slot];
    return result;
}

public MaterialPrimitiveUniform GetMaterialPrimitiveData(uint instanceId)
{
    FInstanceSceneData instance = GetInstanceSceneData(instanceId);
    MaterialPrimitiveUniform result = GetPrimitiveData(instance.PrimitiveId);
    uint first = instanceId * 18;
    result.LocalToWorld = float4x4(GPUSceneInstanceSceneData[first], GPUSceneInstanceSceneData[first + 1],
        GPUSceneInstanceSceneData[first + 2], GPUSceneInstanceSceneData[first + 3]);
    result.WorldToLocal = float4x4(GPUSceneInstanceSceneData[first + 4], GPUSceneInstanceSceneData[first + 5],
        GPUSceneInstanceSceneData[first + 6], GPUSceneInstanceSceneData[first + 7]);
    result.NormalToWorld = float4x4(GPUSceneInstanceSceneData[first + 8], GPUSceneInstanceSceneData[first + 9],
        GPUSceneInstanceSceneData[first + 10], GPUSceneInstanceSceneData[first + 11]);
    result.WorldOriginHigh = GPUSceneInstanceSceneData[first + 12];
    result.WorldOriginLow = GPUSceneInstanceSceneData[first + 13];
    result.BoundsCenter = float4((GPUSceneInstanceSceneData[first + 14].xyz + GPUSceneInstanceSceneData[first + 15].xyz) * 0.5, 1.0);
    result.TransformParams = float4(GPUSceneInstanceSceneData[first + 15].w, 0.0, 0.0, 0.0);
    for (uint slot = 0; slot < 8; ++slot)
        result.CustomInstanceData[slot] = GPUSceneInstancePayloadData[instance.InstancePayloadDataOffset + slot];
    return result;
}

public uint ResolveSceneInstanceId(uint instanceId, uint firstInstance)
{
    return GPUSceneInstanceIds[instanceId + firstInstance];
}
