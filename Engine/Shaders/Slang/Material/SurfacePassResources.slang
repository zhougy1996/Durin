// Surface pass resources are owned by the pass adapter.
import Material.SpecularAntialiasing;
import Lighting.DirectionalShadow;
import Lighting.ViewLightingUniform;
import Lighting.SurfaceLighting;
[[vk::binding(1, 0)]] ConstantBuffer<FViewLightingUniform> Lighting;
[[vk::binding(30, 0)]] StructuredBuffer<FForwardLocalLightUniform> LocalLights;
[[vk::binding(19, 0)]] TextureCube<float4> EnvironmentIrradiance;
[[vk::binding(20, 0)]] TextureCube<float4> EnvironmentPrefiltered;
[[vk::binding(21, 0)]] Texture2D<float4> EnvironmentBrdfLut;
[[vk::binding(22, 0)]] SamplerState EnvironmentSampler;
[[vk::binding(25, 0)]] Texture2DArray<float> DirectionalShadowTexture;
[[vk::binding(26, 0)]] SamplerComparisonState DirectionalShadowSampler;
