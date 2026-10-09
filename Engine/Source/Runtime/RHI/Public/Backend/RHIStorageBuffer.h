#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "RHIBufferUploadData.h"

namespace Durin
{
	// CPU-uploaded shader storage. Backends own the current immutable GPU allocation.
	class FRHIStorageBuffer : public FRHIBuffer
	{
	public:
		RHI_API FRHIStorageBuffer(const FRHIBufferDesc& Desc, ERHIBufferLifetimeUsage Usage,
			FByteView InitialData);
		auto GetLifetimeUsage() const -> ERHIBufferLifetimeUsage { return LifetimeUsage; }
		// Ordered RHI replay only; the CPU copy preserves untouched bytes of partial updates.
		RHI_API virtual auto UpdateContents(uint32 Offset, FByteView Data) -> void;
		auto GetContents() const -> FByteView { return Contents; }
	private:
		const ERHIBufferLifetimeUsage LifetimeUsage;
		// Accounting outlives the storage it measures.
		std::shared_ptr<const FRHIBufferUploadAccounting> Accounting;
		FByteBuffer Contents;
	};

}
