#pragma once

#include "DynamicRHI.h"
#include "RHI.h"
#include "Renderers/ContactShadowRenderer.h"
#include "Renderers/DeferredDirectionalLightingRenderer.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"
#include "Renderers/VolumetricCloudRenderer.h"
#include "Renderers/VolumetricCloudShadowRenderer.h"

namespace Durin::Tests
{
	// Creates exact feature descriptors as test-owned counted RHI targets.
	class FRendererFeatureTargetFixture final
	{
	public:
		static auto CreateGBuffer(uint32 Width, uint32 Height)
			-> std::optional<FGBufferRenderer::FTargets>
		{
			if (Width == 0 || Height == 0
				|| FGBufferRenderer::CalculateTargetBytes(Width, Height)
					> FGBufferRenderer::MaximumRetainedBytes)
				return std::nullopt;
			auto Textures = CreateTextures(FGBufferRenderer::DescribeTargets(
				Width, Height));
			if (!Textures) return std::nullopt;
			return FGBufferRenderer::FTargets{
				.Material = (*Textures)[0], .Normals = (*Textures)[1],
				.Surface = (*Textures)[2], .Emissive = (*Textures)[3]};
		}

		static auto CreateAmbientOcclusion(uint32 Width, uint32 Height,
			EGroundTruthAmbientOcclusionQuality Quality)
			-> std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
		{
			if (Width == 0 || Height == 0
				|| Quality >= EGroundTruthAmbientOcclusionQuality::Count
				|| FGroundTruthAmbientOcclusionRenderer::CalculateTargetBytes(
					Width, Height, Quality)
					> FGroundTruthAmbientOcclusionRenderer::MaximumRetainedBytes)
				return std::nullopt;
			auto Textures = CreateTextures(
				FGroundTruthAmbientOcclusionRenderer::DescribeTargets(
					Width, Height, Quality));
			if (!Textures) return std::nullopt;
			const bool bHalf = Quality
				== EGroundTruthAmbientOcclusionQuality::HalfResolution;
			return FGroundTruthAmbientOcclusionRenderer::FTargets{
				.Raw = (*Textures)[0], .Scratch = (*Textures)[1],
				.Selector = bHalf ? (*Textures)[2] : FTextureRHIRef{},
				.Resolved = bHalf ? (*Textures)[3] : FTextureRHIRef{},
				.Quality = Quality};
		}

		static auto CreateContactFragment(uint32 Width, uint32 Height)
			-> std::optional<FContactShadowVisibilityRenderer::FTargets>
		{
			if (!IsContactExtentValid(Width, Height)) return std::nullopt;
			auto Texture = RHICreateTexture(
				FContactShadowVisibilityRenderer::DescribeFragmentTarget(
					Width, Height));
			return Texture ? std::optional{
				FContactShadowVisibilityRenderer::FTargets{
					.Visibility = std::move(Texture)}} : std::nullopt;
		}

		static auto CreateContactCompute(uint32 Width, uint32 Height)
			-> std::optional<FContactShadowVisibilityRenderer::FComputeTargets>
		{
			if (!IsContactExtentValid(Width, Height)) return std::nullopt;
			return CreateComputeTarget<
				FContactShadowVisibilityRenderer::FComputeTargets>(
				FContactShadowVisibilityRenderer::DescribeComputeTarget(
					Width, Height));
		}

		static auto CreateDeferred(uint32 Width, uint32 Height)
			-> std::optional<FDeferredDirectionalLightingRenderer::FTargets>
		{
			if (Width == 0 || Height == 0
				|| FDeferredDirectionalLightingRenderer::CalculateTargetBytes(
					Width, Height)
					> FDeferredDirectionalLightingRenderer::MaximumRetainedBytes)
				return std::nullopt;
			auto Texture = RHICreateTexture(
				FDeferredDirectionalLightingRenderer::DescribeTarget(Width, Height));
			return Texture ? std::optional{
				FDeferredDirectionalLightingRenderer::FTargets{
					.Color = std::move(Texture)}} : std::nullopt;
		}

		static auto CreateCloudFragment(uint32 Width, uint32 Height)
			-> std::optional<FVolumetricCloudRenderer::FTargets>
		{
			if (!IsCloudExtentValid(Width, Height)) return std::nullopt;
			auto Texture = RHICreateTexture(
				FVolumetricCloudRenderer::DescribeFragmentTarget(Width, Height));
			return Texture ? std::optional{FVolumetricCloudRenderer::FTargets{
				.Cloud = std::move(Texture)}} : std::nullopt;
		}

		static auto CreateCloudCompute(uint32 Width, uint32 Height)
			-> std::optional<FVolumetricCloudRenderer::FComputeTargets>
		{
			if (!IsCloudExtentValid(Width, Height)) return std::nullopt;
			return CreateComputeTarget<FVolumetricCloudRenderer::FComputeTargets>(
				FVolumetricCloudRenderer::DescribeComputeTarget(Width, Height));
		}

		static auto CreateCloudComposite(uint32 Width, uint32 Height)
			-> std::optional<FVolumetricCloudRenderer::FTargets>
		{
			if (!IsCloudExtentValid(Width, Height)) return std::nullopt;
			auto Texture = RHICreateTexture(
				FVolumetricCloudRenderer::DescribeCompositeTarget(Width, Height));
			return Texture ? std::optional{FVolumetricCloudRenderer::FTargets{
				.Cloud = std::move(Texture)}} : std::nullopt;
		}

		static auto CreateCloudShadowFragment(uint32 Width, uint32 Height)
			-> std::optional<FVolumetricCloudShadowRenderer::FTargets>
		{
			if (!IsCloudShadowExtentValid(Width, Height)) return std::nullopt;
			auto Texture = RHICreateTexture(
				FVolumetricCloudShadowRenderer::DescribeFragmentTarget(
					Width, Height));
			return Texture ? std::optional{
				FVolumetricCloudShadowRenderer::FTargets{
					.Visibility = std::move(Texture)}} : std::nullopt;
		}

		static auto CreateCloudShadowCompute(uint32 Width, uint32 Height)
			-> std::optional<FVolumetricCloudShadowRenderer::FComputeTargets>
		{
			if (!IsCloudShadowExtentValid(Width, Height)) return std::nullopt;
			return CreateComputeTarget<
				FVolumetricCloudShadowRenderer::FComputeTargets>(
				FVolumetricCloudShadowRenderer::DescribeComputeTarget(
					Width, Height));
		}

	private:
		template<size_t Count>
		static auto CreateTextures(
			const std::array<FRHITextureCreateDesc, Count>& Descriptions)
			-> std::optional<std::vector<FTextureRHIRef>>
		{
			return CreateTextures(std::span<const FRHITextureCreateDesc>(Descriptions));
		}

		static auto CreateTextures(
			std::span<const FRHITextureCreateDesc> Descriptions)
			-> std::optional<std::vector<FTextureRHIRef>>
		{
			std::vector<FTextureRHIRef> Textures;
			Textures.reserve(Descriptions.size());
			for (const FRHITextureCreateDesc& Description : Descriptions)
			{
				auto Texture = RHICreateTexture(Description);
				if (!Texture) return std::nullopt;
				Textures.push_back(std::move(Texture));
			}
			return Textures;
		}

		template<typename Targets>
		static auto CreateComputeTarget(const FRHITextureCreateDesc& Description)
			-> std::optional<Targets>
		{
			if (GDynamicRHI == nullptr
				|| !GDynamicRHI->RHIIsTextureSupported(Description))
				return std::nullopt;
			auto TargetTexture = RHICreateTexture(Description);
			if (!TargetTexture) return std::nullopt;
			Targets Result;
			if constexpr (requires(Targets Value) { Value.Cloud; })
				Result.Cloud = std::move(TargetTexture);
			else Result.Visibility = std::move(TargetTexture);
			FRHITexture* Texture = [&]() -> FRHITexture* {
				if constexpr (requires(Targets Value) { Value.Cloud; })
					return Result.Cloud;
				else return Result.Visibility;
			}();
			if (Texture == nullptr) return std::nullopt;
			Result.SampledView = GDynamicRHI->RHIGetOrCreateTextureView(Texture,
				MakeDefaultTextureViewDesc(*Texture, ERHITextureViewUsage::Sampled));
			Result.StorageView = GDynamicRHI->RHIGetOrCreateTextureView(Texture,
				MakeDefaultTextureViewDesc(*Texture, ERHITextureViewUsage::Storage));
			return Result.SampledView && Result.StorageView
				? std::optional<Targets>{std::move(Result)} : std::nullopt;
		}

		static auto IsContactExtentValid(uint32 Width, uint32 Height) -> bool
		{
			return Width != 0 && Height != 0
				&& FContactShadowVisibilityRenderer::CalculateTargetBytes(Width, Height)
					<= FContactShadowVisibilityRenderer::MaximumRetainedBytesPerRoute;
		}

		static auto IsCloudExtentValid(uint32 Width, uint32 Height) -> bool
		{
			return Width != 0 && Height != 0
				&& FVolumetricCloudRenderer::FSpatial::CalculateTargetBytes(Width, Height)
					<= FVolumetricCloudRenderer::FSpatial::MaximumRetainedTargetBytesPerFamily;
		}

		static auto IsCloudShadowExtentValid(uint32 Width, uint32 Height) -> bool
		{
			return Width != 0 && Height != 0
				&& FVolumetricCloudShadowRenderer::CalculateTargetBytes(Width, Height)
					<= FVolumetricCloudShadowRenderer::MaximumRetainedBytesPerRoute;
		}
	};
} // namespace Durin::Tests
