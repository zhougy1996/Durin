#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"

#include <Metal/Metal.hpp>

namespace Durin
{
	class FMetalSampler final : public FRHISampler
	{
	public:
		explicit FMetalSampler(NS::SharedPtr<MTL::SamplerState> InSampler)
			: Sampler(std::move(InSampler)) {}
		~FMetalSampler() override
		{
			const FMetalAutoreleasePool Pool;
			Sampler.reset();
		}

		// Borrows the wrapper's native owner; submissions retain the RHI sampler.
		auto GetHandle() const -> MTL::SamplerState* { return Sampler.get(); }
		auto IsImmutable() const -> bool override { return true; }

	private:
		NS::SharedPtr<MTL::SamplerState> Sampler;
	};
}
