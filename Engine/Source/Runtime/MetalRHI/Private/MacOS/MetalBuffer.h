#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"
#include "MetalResourceState.h"

#include <Metal/Metal.hpp>

namespace Durin
{
	class FMetalBuffer final : public FRHIBuffer
	{
	public:
		FMetalBuffer(const FRHIBufferCreateDesc& Desc, NS::SharedPtr<MTL::Buffer> InBuffer)
			: FRHIBuffer(Desc), Buffer(std::move(InBuffer)), StateTracker(Desc.Size) {}
		~FMetalBuffer() override
		{
			const FMetalAutoreleasePool Pool;
			Buffer.reset();
		}

		// Borrows the wrapper's native owner. GPU submissions retain their own owner.
		auto GetHandle() const -> MTL::Buffer* { return Buffer.get(); }

		auto GetStateTracker() -> FMetalAccessStateTracker& { return StateTracker; }

	private:
		NS::SharedPtr<MTL::Buffer> Buffer;
		FMetalAccessStateTracker StateTracker;
	};
}
