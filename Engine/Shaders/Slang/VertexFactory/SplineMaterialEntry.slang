#if DURIN_GENERATED_MATERIAL && DURIN_MATERIAL_VERTEX_STAGE
#define DURIN_DECLARE_BINDINGS_Spline 1
#define DURIN_MATERIAL_BINDING(Role, Name, Type, Reflected, Runtime, Set, Slot, Stride, Stages, Entries) [[vk::binding(Slot, Set)]] Type Name;
#include "../Material/MaterialBindingSchema.inl"
#undef DURIN_MATERIAL_BINDING
#undef DURIN_DECLARE_BINDINGS_Spline

[shader("vertex")]
VSOutput SplineVertexMain(FLocalVertexFactoryInput input, uint instanceId : SV_InstanceID, uint firstInstance : SV_StartInstanceLocation)
{
    return EvaluateGeneratedGeometry(GetSplineMeshVertexFactoryIntermediates(input, SplineMesh), ResolveSceneInstanceId(instanceId, firstInstance));
}


#endif
