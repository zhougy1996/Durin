#pragma once

#import <Metal/Metal.h>

#include "RHIResources.h"

namespace Durin
{
	class FMetalTexture final : public FRHITexture
	{
	public:
		FMetalTexture(const FRHITextureCreateDesc& Desc, id<MTLTexture> InTexture)
			: FRHITexture(Desc), Texture(InTexture) {}

		auto GetHandle() const -> id<MTLTexture> { return Texture; }
		auto GetBackendAllocationBytes() const -> uint64 override
		{ return Texture.allocatedSize; }

	private:
		id<MTLTexture> Texture;
	};

	class FMetalTextureView final : public FRHITextureView
	{
	public:
		FMetalTextureView(FRHITexture* InTexture,
			const FRHITextureViewDesc& InDesc, id<MTLTexture> InView)
			: FRHITextureView(InTexture, InDesc), View(InView) {}

		auto GetHandle() const -> id<MTLTexture> { return View; }

	private:
		id<MTLTexture> View;
	};
}
