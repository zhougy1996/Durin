#include "RHIResources.h"

namespace Doge
{
	// May use a multiple producer single consumer queue here if the contention is high, but currently we don't have that many threads creating resources, so a simple vector with mutex should be fine.
	std::vector<FRHIResource*> PendingDeletes;
	std::mutex PendingDeletesMutex;

#if DO_CHECK
	// This pointer will be set before any FRHIResource being deleted, then it will be checked and reset in the destructor of FRHIResource.
	// This is to catch any unexpected deletion, such as deleting a resource manually without calling DeleteResources.
	thread_local const FRHIResource* CurrentDeleting = nullptr;
#endif

	FRHIResource::FRHIResource(ERHIResourceType InResourceType)
		: ResourceType(InResourceType)
	{
	}

	FRHIResource::~FRHIResource()
	{
#if DO_CHECK
		check(IsEngineExitRequested() || CurrentDeleting == this);
		CurrentDeleting = nullptr;
#endif
	}

	auto FRHIResource::MarkForDelete() const -> void
	{
		if (!AtomicFlags.MarkForDelete(std::memory_order_release))
		{
			std::lock_guard<std::mutex> lock(PendingDeletesMutex);
			PendingDeletes.push_back(const_cast<FRHIResource*>(this));
		}
	}

	auto FRHIResource::DeleteResources(const std::vector<FRHIResource*>& ResourcesToDelete) -> void
	{
		for (FRHIResource* Resource : ResourcesToDelete)
		{
			if (Resource->AtomicFlags.Deleting())
			{
#if DO_CHECK
				CurrentDeleting = Resource;
#endif
				delete Resource;
#if DO_CHECK
				check(CurrentDeleting == nullptr);
#endif
			}
		}
	}

	auto FRHIResource::GatherResourcesToDelete(std::vector<FRHIResource*>& OutResourcesToDelete) -> void
	{
		std::vector<FRHIResource*> LocalTemp;
		{
			std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
			LocalTemp.swap(PendingDeletes);
		}
		OutResourcesToDelete.insert(
			OutResourcesToDelete.end(),
			std::make_move_iterator(LocalTemp.begin()),
			std::make_move_iterator(LocalTemp.end())
		);
	}

} // namespace Doge