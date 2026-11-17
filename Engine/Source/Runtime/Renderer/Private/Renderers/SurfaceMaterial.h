#pragma once

#include "CoreMinimal.h"
#include "GPUSceneParameters.h"

#include "Materials/MaterialRenderProxy.h"
#include "DefaultTextures.h"
#include "RenderResourceCreation.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "Shader/MaterialShader.h"

namespace Durin
{
	struct FShaderBindingState;
	class FDefaultTextureResources;
	struct FSkyLightingGeneration;
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
			PostProcess,
		};

		class FSurfaceParameterProviders
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceParameterProviders)
                DURIN_MATERIAL_PARAMETER(Material, Material, ConstantBuffer<MaterialUniform>, UniformBuffer, UniformBufferDynamic, true, Draw);
#include "Material/SurfaceParameters.inl"
#include "Material/ViewParameters.inl"
#include "Material/PrimitiveParameters.inl"
#include "Material/GPUSceneParameters.inl"
#include "Material/SplineParameters.inl"
#include "Material/HitProxyParameters.inl"
#include "Material/SceneTexturesParameters.inl"
			DURIN_END_SHADER_PARAMETERS();
            static auto GetParametersMetadata() -> const FShaderParametersMetadata*
            { return GetShaderParametersMetadataOrNull<FSurfaceParameterProviders>(); }
		};

        // Storage dispatch is local to this adapter; field identity comes from owner metadata.
        class FCompiledSurfaceBindingLayout
        {
        public:
            enum class EStorage : uint8 { Structure, Texture, Sampler, Collection };
            struct FEntry : FMaterialShaderBinding
            {
                EStorage Storage = EStorage::Structure;
                std::optional<FShaderParameterBinding> StructureBinding;
            };
            FCompiledSurfaceBindingLayout() = default;
            RENDERER_API explicit FCompiledSurfaceBindingLayout(std::span<const FMaterialShaderBinding> Bindings);
            auto WithoutSceneInputs() const -> FCompiledSurfaceBindingLayout
            { auto R = *this; std::erase_if(R.Entries, [](const auto& E) { return E.OwnerName == "SceneTextures"; }); return R; }
            auto IsValid() const -> bool { return bValid; }
            auto GetEntries() const -> std::span<const FEntry> { return Entries; }
            auto ForMaterialParameters(bool Material) const -> FCompiledSurfaceBindingLayout
            { auto R = *this; std::erase_if(R.Entries, [=](const auto& E) { return (E.Scope == EShaderParameterBindingScope::Pass) == Material; }); return R; }
        private:
            bool bValid = false;
            std::vector<FEntry> Entries;
        };

        class FCompiledSurfaceMaterialShader : public FMaterialShader
        {
        public:
            using FMaterialShader::FMaterialShader;
            auto GetSurfaceLayout() const -> const FCompiledSurfaceBindingLayout& { return SurfaceLayout; }
        protected:
            auto OnAcceptedParameterBindings() -> void override
            { SurfaceLayout = FCompiledSurfaceBindingLayout(GetAcceptedParameterBindings()); }
        private:
            FCompiledSurfaceBindingLayout SurfaceLayout;
        };

		class FSurfaceFragmentShader final : public FCompiledSurfaceMaterialShader
		{
		public:
			static auto GetMaterialCompilationDescription() -> FMaterialShaderEntryDescriptor
			{ return {.Entry = EMaterialShaderEntry::Forward, .GeometryUsage = 255, .PassUsage = 1}; }

			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceFragmentShader)
				DURIN_MATERIAL_PARAMETER(Material, Material, ConstantBuffer<MaterialUniform>, UniformBuffer, UniformBufferDynamic, true, Draw);
#include "Material/SurfaceParameters.inl"
#include "Material/ViewParameters.inl"
#include "Material/PrimitiveParameters.inl"
#include "Material/GPUSceneParameters.inl"
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_MATERIAL_SHADER(FSurfaceFragmentShader, FCompiledSurfaceMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"FragmentMain");
		};

		class FSurfaceOpaqueShadowFragmentShader final : public FMaterialShader
		{
		public:
			static auto GetMaterialCompilationDescription() -> FMaterialShaderEntryDescriptor
			{ return {.Entry = EMaterialShaderEntry::Shadow, .GeometryUsage = 255, .PassUsage = 4, .BlendModes = 5}; }

			DURIN_DECLARE_MATERIAL_SHADER(FSurfaceOpaqueShadowFragmentShader, FMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"ShadowFragmentMain");
		};

		class FSurfaceMaskedShadowFragmentShader final : public FCompiledSurfaceMaterialShader
		{
		public:
			static auto GetMaterialCompilationDescription() -> FMaterialShaderEntryDescriptor
			{ return {.Entry = EMaterialShaderEntry::Shadow, .GeometryUsage = 255, .PassUsage = 4, .BlendModes = 2}; }

			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceMaskedShadowFragmentShader)
				DURIN_MATERIAL_PARAMETER(Material, Material, ConstantBuffer<MaterialUniform>, UniformBuffer, UniformBufferDynamic, true, Draw);
#include "Material/ViewParameters.inl"
#include "Material/PrimitiveParameters.inl"
#include "Material/GPUSceneParameters.inl"
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_MATERIAL_SHADER(FSurfaceMaskedShadowFragmentShader, FCompiledSurfaceMaterialShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"ShadowFragmentMain");
		};

		struct FResolvedSurfaceMaterial
		{
			bool bCompiledLayout = false;
			bool bError = false;
			std::vector<FRHITexture*> CompiledTextures;
			std::vector<FRHITexture*> CompiledTextureFallbacks;
			std::vector<FRHISampler*> CompiledSamplers;
			FRHITexture* EnvironmentIrradiance = nullptr;
			FRHITexture* EnvironmentPrefiltered = nullptr;
			FRHITexture* EnvironmentBrdfLut = nullptr;
			FRHISampler* EnvironmentSampler = nullptr;
			FRHITexture* DirectionalShadowTexture = nullptr;
			FRHISampler* DirectionalShadowSampler = nullptr;
		};

		// Resolved material state and the immutable uniform shared by compatible draws.
		struct FPreparedSurfaceMaterial
		{
			FResolvedSurfaceMaterial Surface;
			FRHIUniformBufferRange Uniform;
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

		// Non-owning shader bindings. Sources outlive recording; RHI commands copy their payload.
		struct FMeshDrawShaderBindings
		{
			FRHIShader* Shader = nullptr;
			std::vector<FRHIShaderParameterResource> Parameters;
			// Accepted location of the instance selection provider, when this stage reads it.
			std::optional<std::pair<uint32, uint32>> InstanceIdsAllocation;
			FMeshDrawShaderBindings() = default;
			auto GetShader() const -> FRHIShader* { return Shader; }
			auto GetResources() const -> std::span<const FRHIShaderParameterResource> { return Parameters; }
			RENDERER_API auto SetOnCommandList(FRHICommandList& Commands, FShaderBindingState& State, FRHIBuffer* InstanceBuffer = nullptr, uint32 InstanceDataByteOffset = 0) const -> bool;
			RENDERER_API auto operator==(const FMeshDrawShaderBindings& Other) const -> bool;
			explicit operator bool() const { return Shader != nullptr; }
		};

		RENDERER_API auto PrepareCompiledSurfaceMaterial(
			FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
			const FResolvedSurfaceMaterial& Material,
			const FRHIUniformBufferRange& MaterialBuffer,
			const FRHIUniformBufferRange& Lighting,
			const FRHIUniformBufferRange& HitProxy,
			const FRHIUniformBufferRange& View,
			const FRHIUniformBufferRange& Primitive,
			std::span<const FRHIUniformBufferRange> Collections,
			FMeshDrawShaderBindings& OutBindings, FRHIStorageBufferRange LocalLights = {}, const FGPUSceneShaderParameters& GPUScene = {}) -> bool;
		RENDERER_API auto PrepareCompiledSurfaceMaterial(
			FRHIShader* Shader, const FCompiledSurfaceBindingLayout& Layout,
			const FResolvedSurfaceMaterial& Material,
			const FRHIUniformBufferRange& MaterialBuffer,
			const FRHIUniformBufferRange& Lighting,
			const FRHIUniformBufferRange& HitProxy,
			const FRHIUniformBufferRange& View,
			const FRHIUniformBufferRange& Primitive,
			FMeshDrawShaderBindings& OutBindings) -> bool;

		class FSurfaceMaterialResources final
		{
		public:
			FSurfaceMaterialResources(
				FRendererResourceCoordinator& InCoordinator,
				FDefaultTextureResources& InDefaultTextures);
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
				const std::shared_ptr<const FSkyLightingGeneration>& Environment,
				FRHITexture* DirectionalShadowTexture,
				FRHISampler* DirectionalShadowSampler,
				FResolvedSurfaceMaterial& OutMaterial) const -> bool;
			RENDERER_API auto UpdateUniformExpressionCache_RenderThread(FRHICommandListImmediate& Commands,
				FUniformExpressionCache& Cache, const FMaterialRenderRepresentation& Representation,
				const FMaterialRenderBinding& Binding, std::span<const FRHIUniformBufferRange> Collections) const -> bool;
			auto Prepare_RenderThread(FRHICommandListImmediate& Commands, const FMaterialRenderBinding* Binding,
				ESurfaceMaterialPass Pass, const std::shared_ptr<const FSkyLightingGeneration>& Environment,
				FRHITexture* DirectionalShadowTexture, FRHISampler* DirectionalShadowSampler,
				FPreparedSurfaceMaterial& OutMaterial, const FRHIUniformBufferRange& SharedUniform = {}) const -> bool;
			auto ReleaseResources_RenderThread() -> void;
			auto RefreshPassResources_RenderThread(ESurfaceMaterialPass Pass,
				const std::shared_ptr<const FSkyLightingGeneration>& Environment,
				FRHITexture* DirectionalShadowTexture, FRHISampler* DirectionalShadowSampler,
				FResolvedSurfaceMaterial& Material) const -> bool;
			auto GetSamplerSlotCount_RenderThread() const -> size_t;
			auto GetResourceRevision_RenderThread() const -> uint64;
			auto GetResourceCounters_RenderThread() const
				-> FSurfaceMaterialResourceCounters;

		private:
			struct FState;
			FRendererResourceCoordinator& Coordinator;
			FDefaultTextureResources& DefaultTextures;
			std::unique_ptr<FState> State;
		};

	} // namespace RendererPrivate
} // namespace Durin
