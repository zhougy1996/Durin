DURIN_MATERIAL_PARAMETER(WaterScene, WaterBackgroundColor, Texture2D<float4>, Texture, Texture, false, Pass)
DURIN_MATERIAL_PARAMETER(WaterScene, WaterBackgroundDepth, Texture2D<float>, Texture, Texture, false, Pass)
DURIN_MATERIAL_PARAMETER(WaterScene, WaterCoverage, Texture2D<float>, Texture, Texture, false, Pass)
DURIN_MATERIAL_PARAMETER(WaterScene, WaterView, ConstantBuffer<FWaterViewUniform>, UniformBuffer, UniformBufferDynamic, false, Pass)
