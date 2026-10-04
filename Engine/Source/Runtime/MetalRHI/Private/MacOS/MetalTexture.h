#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"
#include "MetalResourceState.h"

#include <Metal/Metal.hpp>

namespace Durin
{
	class FMetalTexture final : public FRHITexture
	{
	public:
		FMetalTexture(const FRHITextureCreateDesc& Desc, NS::SharedPtr<MTL::Texture> InTexture)
			: FRHITexture(Desc), Texture(std::move(InTexture)),
			  StateTracker(uint64(Desc.NumMips) * Desc.ArraySize * 3) {}
		~FMetalTexture() override
		{
			const FMetalAutoreleasePool Pool;
			Texture.reset();
		}

		// Borrows the wrapper's native owner; submissions retain a separate owner.
		auto GetHandle() const -> MTL::Texture* { return Texture.get(); }
		auto GetBackendAllocationBytes() const -> uint64 override
		{ return Texture->allocatedSize(); }

		auto ValidateAccess(const FRHITextureSubresourceRange& Range, ERHIAccess Expected, ERHIAccess& Tracked) const -> bool
		{
			for (const auto Aspect : {ERHITextureAspect::Color, ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
				if (EnumHasAnyFlags(Range.Aspects, Aspect))
					for (uint32 Layer = Range.FirstArrayLayer; Layer < Range.FirstArrayLayer + Range.NumArrayLayers; ++Layer)
						if (!StateTracker.Validate(StateIndex(Aspect, Range.FirstMip, Layer), Range.NumMips, Expected, Tracked)) return false;
			return true;
		}
		auto ApplyAccess(const FRHITextureSubresourceRange& Range, ERHIAccess Access) -> void
		{
			for (const auto Aspect : {ERHITextureAspect::Color, ERHITextureAspect::Depth, ERHITextureAspect::Stencil})
				if (EnumHasAnyFlags(Range.Aspects, Aspect))
					for (uint32 Layer = Range.FirstArrayLayer; Layer < Range.FirstArrayLayer + Range.NumArrayLayers; ++Layer)
						StateTracker.Apply(StateIndex(Aspect, Range.FirstMip, Layer), Range.NumMips, Access);
		}

	private:
		auto StateIndex(ERHITextureAspect Aspect, uint32 Mip, uint32 Layer) const -> uint64
		{
			const uint64 Plane = Aspect == ERHITextureAspect::Color ? 0 : Aspect == ERHITextureAspect::Depth ? 1 : 2;
			return (Plane * GetArraySize() + Layer) * GetNumMips() + Mip;
		}
		NS::SharedPtr<MTL::Texture> Texture;
		FMetalAccessStateTracker StateTracker;
	};

	class FMetalTextureView final : public FRHITextureView
	{
	public:
		FMetalTextureView(FRHITexture* InTexture,
			const FRHITextureViewDesc& InDesc, NS::SharedPtr<MTL::Texture> InView)
			: FRHITextureView(InTexture, InDesc), View(std::move(InView)) {}
		~FMetalTextureView() override
		{
			const FMetalAutoreleasePool Pool;
			View.reset();
		}

		// Borrows the view's native owner. FRHITextureView retains the source wrapper.
		auto GetHandle() const -> MTL::Texture* { return View.get(); }

	private:
		NS::SharedPtr<MTL::Texture> View;
	};
}
