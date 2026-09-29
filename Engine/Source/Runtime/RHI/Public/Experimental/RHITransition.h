#pragma once

#include "RHIAPI.h"
#include "RHIResources.h"

namespace Durin
{
	struct FRHITransitionDesc final
	{
		std::vector<FRHIBufferTransition> Buffers;
		std::vector<FRHITextureTransition> Textures;
	};

	// Execution-local, single-use begin/end transition metadata. The object owns
	// every referenced resource until both recorded commands and backend use end.
	class FRHITransition
	{
	public:
		RHI_API virtual ~FRHITransition();
		auto GetBufferTransitions() const -> std::span<const FRHIBufferTransition>
		{ return Buffers; }
		auto GetTextureTransitions() const -> std::span<const FRHITextureTransition>
		{ return Textures; }

	protected:
		RHI_API explicit FRHITransition(FRHITransitionDesc Desc);

	private:
		friend class FDynamicRHI;
		std::vector<FRHIBufferTransition> Buffers;
		std::vector<FRHITextureTransition> Textures;
		std::vector<TRefCountPtr<FRHIBuffer>> BufferOwners;
		std::vector<TRefCountPtr<FRHITexture>> TextureOwners;
	};
}
