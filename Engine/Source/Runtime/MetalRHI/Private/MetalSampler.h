#pragma once

#import <Metal/Metal.h>

#include "RHIResources.h"

namespace Durin
{
	class FMetalSampler final : public FRHISampler
	{
	public:
		explicit FMetalSampler(id<MTLSamplerState> InSampler) : Sampler(InSampler) {}

		auto GetHandle() const -> id<MTLSamplerState> { return Sampler; }
		auto IsImmutable() const -> bool override { return true; }

	private:
		id<MTLSamplerState> Sampler;
	};
}
