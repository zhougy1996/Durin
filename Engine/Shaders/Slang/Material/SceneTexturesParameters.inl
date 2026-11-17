// Owner parameter declarations; compiler allocation is deliberately absent.
DURIN_MATERIAL_PARAMETER(SceneTextures, MaterialSceneTextures, ConstantBuffer<FMaterialSceneTextureUniform>, UniformBuffer, UniformBuffer, false, Pass)
DURIN_MATERIAL_PARAMETER(SceneTextures, PostProcessInput0, Texture2D<float4>, Texture, Texture, false, Pass)
DURIN_MATERIAL_PARAMETER(SceneTextures, MaterialSceneDepth, Texture2D<float>, Texture, Texture, true, Pass)
DURIN_MATERIAL_PARAMETER(SceneTextures, MaterialScenePointSampler, SamplerState, Sampler, Sampler, true, Pass)
DURIN_MATERIAL_PARAMETER(SceneTextures, MaterialSceneLinearSampler, SamplerState, Sampler, Sampler, false, Pass)
