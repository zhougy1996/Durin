#include "Experimental/RHITransition.h"

namespace Durin
{
	FRHITransition::FRHITransition(FRHITransitionDesc Desc)
		: Buffers(std::move(Desc.Buffers)), Textures(std::move(Desc.Textures))
	{
		BufferOwners.reserve(Buffers.size());
		for (const auto& Transition : Buffers) BufferOwners.emplace_back(Transition.Buffer);
		TextureOwners.reserve(Textures.size());
		for (const auto& Transition : Textures) TextureOwners.emplace_back(Transition.Texture);
	}

	FRHITransition::~FRHITransition() = default;
}
