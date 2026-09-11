#pragma once
#include "RHICompletion.h"
#include "RHIResources.h"

namespace Durin
{
	struct FRHIQueueTransferDesc
	{
		FRHIQueueId Source;
		FRHIQueueId Destination;
		std::vector<FRHIBufferTransition> Buffers;
		std::vector<FRHITextureTransition> Textures;
	};

	// Backend-owned, single-use release/acquire pair. Shared command ownership is
	// independent of CPU replay completion and native queue completion tickets.
	class FRHIQueueTransfer
	{
	public:
		RHI_API virtual ~FRHIQueueTransfer();
		auto GetSourceQueue() const -> FRHIQueueId { return Source; }
		auto GetDestinationQueue() const -> FRHIQueueId { return Destination; }
	protected:
		FRHIQueueTransfer(FRHIQueueId InSource, FRHIQueueId InDestination)
			: Source(InSource), Destination(InDestination) {}
	private:
		const FRHIQueueId Source, Destination;
	};
}
