// Owner parameter declarations; compiler allocation is deliberately absent.
DURIN_MATERIAL_PARAMETER(Scene, GPUScenePrimitiveSceneData, StructuredBuffer<float4>, StorageBuffer, StorageBuffer, true, Pass)
DURIN_MATERIAL_PARAMETER(Scene, GPUSceneInstanceSceneData, StructuredBuffer<float4>, StorageBuffer, StorageBuffer, true, Pass)
DURIN_MATERIAL_PARAMETER(Scene, GPUSceneInstancePayloadData, StructuredBuffer<float4>, StorageBuffer, StorageBuffer, true, Pass)
DURIN_MATERIAL_PARAMETER(Scene, GPUSceneInstanceIds, StructuredBuffer<uint>, StorageBuffer, StorageBuffer, true, Pass)
