#if !DURIN_GENERATED_MATERIAL
#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage)
#include "../Material/SplineParameters.inl"
#undef DURIN_MATERIAL_PARAMETER
#endif
#if DURIN_GENERATED_MATERIAL && DURIN_MATERIAL_VERTEX_STAGE
#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage) Type Name;
#include "../Material/SplineParameters.inl"
#undef DURIN_MATERIAL_PARAMETER

[shader("vertex")]
VSOutput SplineVertexMain(FLocalVertexFactoryInput input, uint instanceId : SV_InstanceID, uint firstInstance : SV_StartInstanceLocation)
{
    return EvaluateGeneratedGeometry(GetSplineMeshVertexFactoryIntermediates(input, SplineMesh), ResolveSceneInstanceId(instanceId, firstInstance));
}


#endif
