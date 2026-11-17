// Single logical material binding authority. Intentionally supports repeated inclusion.

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_View)
DURIN_MATERIAL_BINDING(View, MaterialView, ConstantBuffer<MaterialViewUniform>, UniformBuffer, UniformBufferDynamic, 0, 0, 0, 3, 127)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_Primitive)
DURIN_MATERIAL_BINDING(Primitive, MaterialPrimitive, ConstantBuffer<MaterialPrimitiveUniform>, UniformBuffer, UniformBufferDynamic, 1, 0, 0, 3, 63)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_Material)
DURIN_MATERIAL_BINDING(Material, Material, ConstantBuffer<MaterialUniform>, UniformBuffer, UniformBufferDynamic, 1, 2, 0, 3, 127)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_Indexed)
DURIN_MATERIAL_BINDING(Collection, MaterialCollection, ConstantBuffer<MaterialCollectionUniform>, UniformBuffer, UniformBuffer, 1, 3, 1, 3, 127)
DURIN_MATERIAL_BINDING(Texture, MaterialTexture, Texture2D<float4>, Texture, Texture, 1, 32, 2, 3, 127)
DURIN_MATERIAL_BINDING(Sampler, MaterialSampler, SamplerState, Sampler, Sampler, 1, 33, 2, 3, 127)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_Surface)
DURIN_MATERIAL_BINDING(Lighting, Lighting, ConstantBuffer<FViewLightingUniform>, UniformBuffer, UniformBufferDynamic, 0, 1, 0, 2, 1)
DURIN_MATERIAL_BINDING(LocalLights, LocalLights, StructuredBuffer<FForwardLocalLightUniform>, StorageBuffer, StorageBuffer, 0, 30, 0, 2, 1)
DURIN_MATERIAL_BINDING(EnvironmentIrradiance, EnvironmentIrradiance, TextureCube<float4>, Texture, Texture, 0, 19, 0, 2, 1)
DURIN_MATERIAL_BINDING(EnvironmentPrefiltered, EnvironmentPrefiltered, TextureCube<float4>, Texture, Texture, 0, 20, 0, 2, 1)
DURIN_MATERIAL_BINDING(EnvironmentBrdfLut, EnvironmentBrdfLut, Texture2D<float4>, Texture, Texture, 0, 21, 0, 2, 1)
DURIN_MATERIAL_BINDING(EnvironmentSampler, EnvironmentSampler, SamplerState, Sampler, Sampler, 0, 22, 0, 2, 1)
DURIN_MATERIAL_BINDING(DirectionalShadowTexture, DirectionalShadowTexture, Texture2DArray<float>, Texture, Texture, 0, 25, 0, 2, 1)
DURIN_MATERIAL_BINDING(DirectionalShadowSampler, DirectionalShadowSampler, SamplerComparisonState, Sampler, Sampler, 0, 26, 0, 2, 1)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_GPUScene)
DURIN_MATERIAL_BINDING(GPUScenePrimitive, GPUScenePrimitiveSceneData, StructuredBuffer<float4>, StorageBuffer, StorageBuffer, 0, 27, 0, 3, 63)
DURIN_MATERIAL_BINDING(GPUSceneInstance, GPUSceneInstanceSceneData, StructuredBuffer<float4>, StorageBuffer, StorageBuffer, 0, 28, 0, 3, 63)
DURIN_MATERIAL_BINDING(GPUScenePayload, GPUSceneInstancePayloadData, StructuredBuffer<float4>, StorageBuffer, StorageBuffer, 0, 29, 0, 3, 63)
DURIN_MATERIAL_BINDING(GPUSceneInstanceIds, GPUSceneInstanceIds, StructuredBuffer<uint>, StorageBuffer, StorageBuffer, 0, 31, 0, 3, 63)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_Spline)
DURIN_MATERIAL_BINDING(Spline, SplineMesh, ConstantBuffer<FSplineMeshUniform>, UniformBuffer, UniformBufferDynamic, 1, 24, 0, 1, 32)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_HitProxy)
DURIN_MATERIAL_BINDING(HitProxy, HitProxy, ConstantBuffer<HitProxyUniform>, UniformBuffer, UniformBufferDynamic, 1, 27, 0, 2, 8)
#endif

#if defined(DURIN_MATERIAL_BINDING_SCHEMA) || defined(DURIN_DECLARE_BINDINGS_SceneTextures)
DURIN_MATERIAL_BINDING(SceneUniform, MaterialSceneTextures, ConstantBuffer<FMaterialSceneTextureUniform>, UniformBuffer, UniformBuffer, 0, 40, 0, 2, 64)
DURIN_MATERIAL_BINDING(SceneInput0, PostProcessInput0, Texture2D<float4>, Texture, Texture, 0, 41, 0, 2, 64)
DURIN_MATERIAL_BINDING(SceneDepth, MaterialSceneDepth, Texture2D<float>, Texture, Texture, 0, 42, 0, 2, 64)
DURIN_MATERIAL_BINDING(ScenePointSampler, MaterialScenePointSampler, SamplerState, Sampler, Sampler, 0, 43, 0, 2, 64)
DURIN_MATERIAL_BINDING(SceneLinearSampler, MaterialSceneLinearSampler, SamplerState, Sampler, Sampler, 0, 44, 0, 2, 64)
#endif
