#pragma once

#include "Renderers/AmbientOcclusionRendering.h"
#include "Renderers/GBufferRenderer.h"
#include "Renderers/GroundTruthAmbientOcclusionRenderer.h"

namespace Durin::SceneTextureGroups
{
	// These helpers borrow the original parameter members; resolver authority is address-based.
	template<typename Wrapper>
	auto ResolveTexture(const FRDGParameterResolver& Resolver,
		const std::optional<Wrapper>& Parameter) -> FRHITexture*
	{
		if constexpr (std::same_as<Wrapper, FRDGColorAttachmentParameter>)
			return Resolver.GetColorAttachment(Parameter).Texture;
		else
			return Resolver.GetTexture(Parameter);
	}

	template<typename Wrapper>
	auto FillGBuffer(const std::optional<FGBufferTextureHandles>& Textures,
		std::optional<Wrapper>& Material, std::optional<Wrapper>& Normals,
		std::optional<Wrapper>& Surface, std::optional<Wrapper>& Emissive) -> void
	{
		const std::array Members{&Material, &Normals, &Surface, &Emissive};
		for (size_t Index = 0; Index < Members.size(); ++Index)
		{
			Members[Index]->reset();
			if (Textures)
				*Members[Index] = Wrapper{Textures->Colors[Index],
					{ERHITextureAspect::Color, 0, 1, 0, 1}};
		}
	}

	template<typename Wrapper>
	auto FillGBuffer(const std::optional<FGBufferTextureHandles>& Textures,
		std::array<std::optional<Wrapper>, 4>& Parameters) -> void
	{
		FillGBuffer(Textures, Parameters[0], Parameters[1], Parameters[2], Parameters[3]);
	}

	template<typename Wrapper>
	auto ResolveGBuffer(const FRDGParameterResolver& Resolver,
		const std::optional<Wrapper>& Material, const std::optional<Wrapper>& Normals,
		const std::optional<Wrapper>& Surface, const std::optional<Wrapper>& Emissive)
		-> std::optional<FGBufferRenderer::FTargets>
	{
		FRHITexture* MaterialTexture = ResolveTexture(Resolver, Material);
		if (MaterialTexture == nullptr) return std::nullopt;
		return FGBufferRenderer::FTargets{
			.Material = MaterialTexture, .Normals = ResolveTexture(Resolver, Normals),
			.Surface = ResolveTexture(Resolver, Surface),
			.Emissive = ResolveTexture(Resolver, Emissive)};
	}

	template<typename Wrapper>
	auto ResolveGBuffer(const FRDGParameterResolver& Resolver,
		const std::array<std::optional<Wrapper>, 4>& Parameters)
		-> std::optional<FGBufferRenderer::FTargets>
	{
		return ResolveGBuffer(Resolver, Parameters[0], Parameters[1], Parameters[2], Parameters[3]);
	}

	// Raw/Scratch are required together; the reconstruction pair is optional together.
	template<typename Wrapper>
	auto FillAmbientOcclusion(const FAmbientOcclusionTextureHandles& Textures,
		std::array<std::optional<Wrapper>, 4>& Parameters) -> void
	{
		const FRHITextureSubresourceRange Range{ERHITextureAspect::Color, 0, 1, 0, 1};
		Parameters = {};
		Parameters[0] = Wrapper{Textures.Raw, Range};
		Parameters[1] = Wrapper{Textures.Scratch, Range};
		if (Textures.HalfResolution)
		{
			Parameters[2] = Wrapper{Textures.HalfResolution->Selector, Range};
			Parameters[3] = Wrapper{Textures.HalfResolution->Resolved, Range};
		}
	}

	template<typename Wrapper>
	auto ResolveAmbientOcclusion(const FRDGParameterResolver& Resolver,
		const std::array<std::optional<Wrapper>, 4>& Parameters,
		EGroundTruthAmbientOcclusionQuality Quality)
		-> FGroundTruthAmbientOcclusionRenderer::FTargets
	{
		return {.Raw = ResolveTexture(Resolver, Parameters[0]),
			.Scratch = ResolveTexture(Resolver, Parameters[1]),
			.Selector = ResolveTexture(Resolver, Parameters[2]),
			.Resolved = ResolveTexture(Resolver, Parameters[3]), .Quality = Quality};
	}

	template<typename Wrapper>
	auto ResolveOptionalAmbientOcclusion(const FRDGParameterResolver& Resolver,
		const std::array<std::optional<Wrapper>, 4>& Parameters,
		EGroundTruthAmbientOcclusionQuality Quality)
		-> std::optional<FGroundTruthAmbientOcclusionRenderer::FTargets>
	{
		if (!Parameters[0]) return std::nullopt;
		return ResolveAmbientOcclusion(Resolver, Parameters, Quality);
	}
}
