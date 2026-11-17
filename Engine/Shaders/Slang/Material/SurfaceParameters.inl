// Owner parameter declarations; compiler allocation is deliberately absent.
DURIN_MATERIAL_PARAMETER(Pass, Lighting, ConstantBuffer<FViewLightingUniform>, UniformBuffer, UniformBufferDynamic, true, Pass)
DURIN_MATERIAL_PARAMETER(Pass, LocalLights, StructuredBuffer<FForwardLocalLightUniform>, StorageBuffer, StorageBuffer, true, Pass)
DURIN_MATERIAL_PARAMETER(Pass, EnvironmentIrradiance, TextureCube<float4>, Texture, Texture, true, Pass)
DURIN_MATERIAL_PARAMETER(Pass, EnvironmentPrefiltered, TextureCube<float4>, Texture, Texture, true, Pass)
DURIN_MATERIAL_PARAMETER(Pass, EnvironmentBrdfLut, Texture2D<float4>, Texture, Texture, true, Pass)
DURIN_MATERIAL_PARAMETER(Pass, EnvironmentSampler, SamplerState, Sampler, Sampler, true, Pass)
DURIN_MATERIAL_PARAMETER(Pass, DirectionalShadowTexture, Texture2DArray<float>, Texture, Texture, true, Pass)
DURIN_MATERIAL_PARAMETER(Pass, DirectionalShadowSampler, SamplerComparisonState, Sampler, Sampler, true, Pass)
