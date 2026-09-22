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

		class FCompiledSurfaceBindingLayout
		{
		public:
			enum class ESource : uint8
			{
				View, Lighting, Material, HitProxy, Texture, Sampler,
				EnvironmentIrradiance, EnvironmentPrefiltered, EnvironmentBrdfLut,
				EnvironmentSampler, DirectionalShadowTexture, DirectionalShadowSampler
			};
			struct FEntry
			{
				uint32 SetIndex = 0;
				uint32 BindingIndex = 0;
				ERHIBindingType Type = ERHIBindingType::Texture;
				ESource Source = ESource::Texture;
				uint32 ResourceIndex = 0;
			};
			RENDERER_API explicit FCompiledSurfaceBindingLayout(const FShaderReflectionData& Reflection);
			auto IsValid() const -> bool { return bValid; }
			auto GetEntries() const -> std::span<const FEntry> { return Entries; }
		private:
			bool bValid = false;
			std::vector<FEntry> Entries;
		};

		// Typed surface shaders compile their resource mapping at instance creation.
		class FCompiledSurfaceMaterialShader : public FMaterialShader
		{
		public:
			FCompiledSurfaceMaterialShader(const FShaderType* Type, FShaderMapBase* Map,
				const FShaderReflectionData& Reflection)
				: FMaterialShader(Type, Map, Reflection), SurfaceLayout(Reflection) {}
			auto GetSurfaceLayout() const -> const FCompiledSurfaceBindingLayout& { return SurfaceLayout; }
		private:
			FCompiledSurfaceBindingLayout SurfaceLayout;
		};

		class FSurfaceFragmentShader final : public FCompiledSurfaceMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Lighting);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MeshView);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(EnvironmentIrradiance);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(EnvironmentPrefiltered);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(EnvironmentBrdfLut);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(EnvironmentSampler);
				DURIN_SHADER_PARAMETER_TEXTURE_OPTIONAL(DirectionalShadowTexture);
				DURIN_SHADER_PARAMETER_SAMPLER_OPTIONAL(DirectionalShadowSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_MATERIAL_SHADER(FSurfaceFragmentShader, FCompiledSurfaceMaterialShader,
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

		class FSurfaceMaskedShadowFragmentShader final : public FCompiledSurfaceMaterialShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceMaskedShadowFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(Material);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC_OPTIONAL(MeshView);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_MATERIAL_SHADER(FSurfaceMaskedShadowFragmentShader, FCompiledSurfaceMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"ShadowFragmentMain");
		};

		struct FResolvedSurfaceMaterial
		{
			bool bCompiledLayout = false;
			FMaterialRenderBinding PublishedBinding;
			FByteView CompiledUniformPayload;
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

		// Prepared before recording; retains every resource referenced by the span.
		class FPreparedSurfaceMaterialBindings
		{
		public:
			auto GetShader() const -> FRHIShader* { return Batch ? Batch->GetShader() : nullptr; }
			auto GetResources() const -> std::span<const FRHIShaderParameterResource>
			{
				return Batch ? Batch->GetParameters() : std::span<const FRHIShaderParameterResource>{};
			}
			RENDERER_API auto Bind(FRHICommandList& CommandList) const -> bool;
		private:
			friend RENDERER_API auto PrepareCompiledSurfaceMaterial(FRHIShader*, const FCompiledSurfaceBindingLayout&,
				const FResolvedSurfaceMaterial&, const FRHIUniformBufferRange&, const FRHIUniformBufferRange&,
				const FRHIUniformBufferRange&, const FRHIUniformBufferRange&, FPreparedSurfaceMaterialBindings&) -> bool;
			std::shared_ptr<const FRHIShaderParameterBatch> Batch;
		};

		RENDERER_API auto PrepareCompiledSurfaceMaterial(
			FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
			const FResolvedSurfaceMaterial& Material,
			const FRHIUniformBufferRange& MaterialBuffer,
			const FRHIUniformBufferRange& Lighting,
			const FRHIUniformBufferRange& HitProxy,
			const FRHIUniformBufferRange& View,
			FPreparedSurfaceMaterialBindings& OutBindings) -> bool;

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
