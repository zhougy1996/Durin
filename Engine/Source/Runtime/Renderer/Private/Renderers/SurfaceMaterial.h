#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "DefaultTextures.h"
#include "RenderResourceCreation.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "Shader/MaterialShader.h"

#include <array>
#include <cstddef>
#include <memory>

namespace Durin
{
	class FDefaultTextureResources;
	class FEnvironmentLightingResources;
	class FGBufferRenderer;
	class FRendererResourceCoordinator;

	namespace RendererPrivate
	{
		enum class ESurfaceMaterialPass : uint8
		{
			OpaqueShadow,
			MaskedShadow,
			Forward,
			GBuffer,
		};

		class FSurfaceFragmentShader final : public FMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Lighting);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(EnvironmentIrradiance);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(EnvironmentPrefiltered);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(EnvironmentBrdfLut);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(EnvironmentSampler);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(DirectionalShadowTexture);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(DirectionalShadowSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_MATERIAL_SHADER(FSurfaceFragmentShader, FMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"FragmentMain");
		};

		class FSurfaceOpaqueShadowFragmentShader final : public FMeshMaterialShader
		{
		public:
			DURIN_DECLARE_MESH_MATERIAL_SHADER(FSurfaceOpaqueShadowFragmentShader, FMeshMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"OpaqueShadowFragmentMain");
		};

		class FSurfaceMaskedShadowFragmentShader final : public FMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceMaskedShadowFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_MATERIAL_SHADER(FSurfaceMaskedShadowFragmentShader, FMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"ShadowFragmentMain");
		};

		struct FResolvedSurfaceMaterial
		{
			bool bCompiledLayout = false;
			FByteBuffer CompiledUniformPayload;
			std::vector<FRHITexture*> CompiledTextures;
			std::vector<FRHISampler*> CompiledSamplers;
			FRHITexture* EnvironmentIrradiance = nullptr;
			FRHITexture* EnvironmentPrefiltered = nullptr;
			FRHITexture* EnvironmentBrdfLut = nullptr;
			FRHISampler* EnvironmentSampler = nullptr;
			FRHITexture* DirectionalShadowTexture = nullptr;
			FRHISampler* DirectionalShadowSampler = nullptr;
		};

		struct FSurfaceMaterialResourceCounters
		{
			size_t SamplerLookups = 0;
			size_t SamplerCreations = 0;
			size_t SamplerReuses = 0;
			size_t SamplerFailures = 0;

			auto IsConserved() const -> bool
			{
				return SamplerLookups == SamplerCreations + SamplerReuses
					+ SamplerFailures;
			}
		};

		auto BindCompiledSurfaceMaterial(
			FRHICommandListImmediate& CommandList, FRHIShader* Shader,
			const FShaderReflectionData& Reflection,
			const FResolvedSurfaceMaterial& Material,
			const FRHIUniformBufferRange& MaterialBuffer,
			const FRHIUniformBufferRange& Lighting = {}) -> bool;

		class FSurfaceMaterialResources final
		{
		public:
			FSurfaceMaterialResources(
				FRendererResourceCoordinator& InCoordinator,
				FDefaultTextureResources& InDefaultTextures,
				FEnvironmentLightingResources& InEnvironmentLighting);
			~FSurfaceMaterialResources();

			FSurfaceMaterialResources(const FSurfaceMaterialResources&) = delete;
			auto operator=(const FSurfaceMaterialResources&)
				-> FSurfaceMaterialResources& = delete;

			auto Ensure_RenderThread(
				const FMaterialRenderBinding& Binding,
				ESurfaceMaterialPass Pass) -> bool;
			auto Resolve_RenderThread(
				const FMaterialRenderBinding& Binding,
				ESurfaceMaterialPass Pass,
				bool bLit,
				bool bEnableSpecularAA,
				FRHITexture* DirectionalShadowTexture,
				FRHISampler* DirectionalShadowSampler,
				FResolvedSurfaceMaterial& OutMaterial) const -> bool;
			auto ReleaseResources_RenderThread() -> void;
			auto GetSamplerSlotCount_RenderThread() const -> size_t;
			auto GetResourceCounters_RenderThread() const
				-> FSurfaceMaterialResourceCounters;

		private:
			struct FState;
			FRendererResourceCoordinator& Coordinator;
			FDefaultTextureResources& DefaultTextures;
			FEnvironmentLightingResources& EnvironmentLighting;
			std::unique_ptr<FState> State;
		};

	} // namespace RendererPrivate
} // namespace Durin
