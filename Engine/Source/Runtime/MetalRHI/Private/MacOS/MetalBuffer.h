#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"
#include "MetalResourceState.h"

#include <Metal/Metal.hpp>

namespace Durin
{
	// Immutable native parameter data retained by bindings and GPU submissions.
	struct FMetalUniformAllocation
	{
		NS::SharedPtr<MTL::Buffer> Handle;
		std::vector<TRefCountPtr<FRHIResource>> References;
		~FMetalUniformAllocation()
		{
			const FMetalAutoreleasePool Pool;
			Handle.reset();
		}
	};

	// Uniform updates replace the allocation; bindings retain the allocation they captured.
	class FMetalUniformBuffer final : public FRHIUniformBuffer
	{
	public:
		FMetalUniformBuffer(MTL::Device* InDevice, const FRHIUniformBufferLayout& Layout,
			ERHIBufferLifetimeUsage Usage, FByteView Data, std::span<FRHIResource* const> References)
			: FRHIUniformBuffer(Layout, Usage, Data, References), Device(NS::RetainPtr(InDevice)) {}
		auto UpdateContents(FByteView Data, std::span<FRHIResource* const> References) -> void override;
		auto GetAllocation() -> const std::shared_ptr<FMetalUniformAllocation>&;
	private:
		NS::SharedPtr<MTL::Device> Device;
		std::shared_ptr<FMetalUniformAllocation> Allocation;
	};

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
