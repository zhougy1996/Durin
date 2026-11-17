struct FM15VFUniform { float4 Offset; };
#define DURIN_MATERIAL_PARAMETER(Owner, Name, Type, Reflected, Runtime, Optional, Usage) Type Name;
#include "M15VFParameters.inl"
#undef DURIN_MATERIAL_PARAMETER
#if DURIN_GENERATED_MATERIAL && DURIN_MATERIAL_VERTEX_STAGE
[shader("vertex")]
VSOutput VertexMain(FLocalVertexFactoryInput input, uint instanceId : SV_InstanceID, uint firstInstance : SV_StartInstanceLocation)
{
    VSOutput output = EvaluateGeneratedGeometry(GetLocalVertexFactoryIntermediates(input), ResolveSceneInstanceId(instanceId, firstInstance));
    output.pos.xyz += M15Offset.Offset.xyz;
    return output;
}
#endif
