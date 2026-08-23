#pragma once

#include "Materials/MaterialRenderProxy.h"
#include "DefaultTextures.h"
#include "RenderResourceCreation.h"
#include "RHICommandList.h"
#include "RHIResources.h"
#include "Shader/Shader.h"

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
		inline constexpr size_t SurfaceMaterialRoleCount = 8;
		inline constexpr std::array<EDefaultTexture, SurfaceMaterialRoleCount>
			SurfaceTextureFallbacks{
				EDefaultTexture::White,
				EDefaultTexture::FlatNormal,
				EDefaultTexture::White,
				EDefaultTexture::White,
				EDefaultTexture::White,
				EDefaultTexture::Black,
				EDefaultTexture::White,
				EDefaultTexture::White,
			};

		enum class ESurfaceMaterialPass : uint8
		{
			OpaqueShadow,
			MaskedShadow,
			Forward,
			GBuffer,
		};

		constexpr auto GetSurfaceMaterialRequiredRoleMask(
			ESurfaceMaterialPass Pass) -> uint8
		{
			switch (Pass)
			{
			case ESurfaceMaterialPass::OpaqueShadow: return 0;
			case ESurfaceMaterialPass::MaskedShadow: return uint8{1} << 7;
			case ESurfaceMaterialPass::Forward:
			case ESurfaceMaterialPass::GBuffer: return 0xff;
			}
			return 0;
		}

		struct FSurfaceMaterialUniform
		{
			FVector4f BaseColor{1.0f};
			FVector4f EmissiveMetallic{0.0f};
			FVector4f NormalRoughness{0.0f, 0.0f, 1.0f, 0.5f};
			FVector4f SurfaceParams{1.0f, 1.0f, 1.0f, 0.0f};
			std::array<FVector4f, SurfaceMaterialRoleCount> UVTransforms{};
			FVector4f UVChannels0{0.0f};
			FVector4f UVChannels1{0.0f};
			FVector4f UVRotations0{0.0f};
			FVector4f UVRotations1{0.0f};
		};

		static_assert(sizeof(FSurfaceMaterialUniform) == 256);
		static_assert(alignof(FSurfaceMaterialUniform) == alignof(FVector4f));
		static_assert(offsetof(FSurfaceMaterialUniform, UVTransforms) == 64);

		inline auto MakeSurfaceMaterialUniform(
			const FMaterialRenderBinding& Binding,
			bool bLit,
			bool bEnableSpecularAA) -> FSurfaceMaterialUniform
		{
			FSurfaceMaterialUniform Result;
			Result.BaseColor = Binding.BaseColor;
			Result.EmissiveMetallic = FVector4f(Binding.Emissive, Binding.Metallic);
			Result.NormalRoughness = FVector4f(Binding.Normal, Binding.Roughness);
			Result.SurfaceParams = FVector4f(
				Binding.AmbientOcclusion, Binding.OpacityMask,
				bLit ? 1.0f : 0.0f,
				bLit && bEnableSpecularAA ? 1.0f : 0.0f);
			for (size_t Role = 0; Role < SurfaceMaterialRoleCount; ++Role)
			{
				Result.UVTransforms[Role] = FVector4f(
					Binding.UVScales[Role].x, Binding.UVScales[Role].y,
					Binding.UVOffsets[Role].x, Binding.UVOffsets[Role].y);
			}
			Result.UVChannels0 = FVector4f(Binding.UVChannels[0], Binding.UVChannels[1], Binding.UVChannels[2], Binding.UVChannels[3]);
			Result.UVChannels1 = FVector4f(Binding.UVChannels[4], Binding.UVChannels[5], Binding.UVChannels[6], Binding.UVChannels[7]);
			Result.UVRotations0 = FVector4f(Binding.UVRotations[0], Binding.UVRotations[1], Binding.UVRotations[2], Binding.UVRotations[3]);
			Result.UVRotations1 = FVector4f(Binding.UVRotations[4], Binding.UVRotations[5], Binding.UVRotations[6], Binding.UVRotations[7]);
			return Result;
		}

		class FSurfaceFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Lighting);
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(BaseColorTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(NormalTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(MetallicTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(RoughnessTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(AmbientOcclusionTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(EmissiveTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityTexture);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(BaseColorSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(NormalSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(MetallicSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(RoughnessSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(AmbientOcclusionSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(EmissiveSampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacitySampler);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacityMaskSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentIrradiance);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentPrefiltered);
				DURIN_SHADER_PARAMETER_TEXTURE(EnvironmentBrdfLut);
				DURIN_SHADER_PARAMETER_SAMPLER(EnvironmentSampler);
				DURIN_SHADER_PARAMETER_TEXTURE(DirectionalShadowTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(DirectionalShadowSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FSurfaceFragmentShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"FragmentMain");
		};

		class FSurfaceOpaqueShadowFragmentShader final : public FShader
		{
		public:
			DURIN_DECLARE_SHADER(FSurfaceOpaqueShadowFragmentShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"OpaqueShadowFragmentMain");
		};

		class FSurfaceMaskedShadowFragmentShader final : public FShader
		{
		public:
			DURIN_BEGIN_SHADER_PARAMETERS(FSurfaceMaskedShadowFragmentShader)
				DURIN_SHADER_PARAMETER_UNIFORM_BUFFER_DYNAMIC(Material);
				DURIN_SHADER_PARAMETER_TEXTURE(OpacityMaskTexture);
				DURIN_SHADER_PARAMETER_SAMPLER(OpacityMaskSampler);
			DURIN_END_SHADER_PARAMETERS();

			DURIN_DECLARE_SHADER(FSurfaceMaskedShadowFragmentShader, FShader,
				"/Engine/StaticMeshBasePass", EShaderFrequency::Fragment,
				"ShadowFragmentMain");
		};

		struct FResolvedSurfaceMaterial
		{
			FSurfaceMaterialUniform Uniform;
			std::array<FRHITexture*, SurfaceMaterialRoleCount> Textures{};
			std::array<FRHISampler*, SurfaceMaterialRoleCount> Samplers{};
			FRHITexture* EnvironmentIrradiance = nullptr;
			FRHITexture* EnvironmentPrefiltered = nullptr;
			FRHITexture* EnvironmentBrdfLut = nullptr;
			FRHISampler* EnvironmentSampler = nullptr;
			FRHITexture* DirectionalShadowTexture = nullptr;
			FRHISampler* DirectionalShadowSampler = nullptr;
			uint8 ResolvedRoleMask = 0;
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

		auto MakeSurfaceForwardParameters(
			const FResolvedSurfaceMaterial& Material,
			const FRHIUniformBufferRange& MaterialBuffer,
			const FRHIUniformBufferRange& Lighting)
			-> FSurfaceFragmentShader::FParameters;

		auto MakeSurfaceMaskedShadowParameters(
			const FResolvedSurfaceMaterial& Material,
			const FRHIUniformBufferRange& MaterialBuffer)
			-> FSurfaceMaskedShadowFragmentShader::FParameters;
	} // namespace RendererPrivate
} // namespace Durin
