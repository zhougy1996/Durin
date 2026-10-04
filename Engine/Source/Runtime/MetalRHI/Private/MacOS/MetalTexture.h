#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"

#include <Metal/Metal.hpp>

namespace Durin
{
	class FMetalTexture final : public FRHITexture
	{
	public:
		FMetalTexture(const FRHITextureCreateDesc& Desc, NS::SharedPtr<MTL::Texture> InTexture)
			: FRHITexture(Desc), Texture(std::move(InTexture)) {}
		~FMetalTexture() override
		{
			const FMetalAutoreleasePool Pool;
			Texture.reset();
		}

		// Borrows the wrapper's native owner; submissions retain a separate owner.
		auto GetHandle() const -> MTL::Texture* { return Texture.get(); }
		auto GetBackendAllocationBytes() const -> uint64 override
		{ return Texture->allocatedSize(); }

	private:
		NS::SharedPtr<MTL::Texture> Texture;
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
