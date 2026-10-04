#pragma once

#import <Metal/Metal.h>

#include "RHIResources.h"

namespace Durin
{
	class FMetalBuffer final : public FRHIBuffer
	{
	public:
		FMetalBuffer(const FRHIBufferCreateDesc& Desc, id<MTLBuffer> InBuffer)
			: FRHIBuffer(Desc), Buffer(InBuffer) {}

		auto GetHandle() const -> id<MTLBuffer> { return Buffer; }

	private:
		id<MTLBuffer> Buffer;
	};
}
