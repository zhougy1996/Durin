#if DURIN_GENERATED_MATERIAL && DURIN_MATERIAL_VERTEX_STAGE
[shader("vertex")]
VSOutput VertexMain(FLocalVertexFactoryInput input, uint instanceId : SV_InstanceID, uint firstInstance : SV_StartInstanceLocation)
{
    return EvaluateGeneratedGeometry(GetLocalVertexFactoryIntermediates(input), ResolveSceneInstanceId(instanceId, firstInstance));
}

#endif
