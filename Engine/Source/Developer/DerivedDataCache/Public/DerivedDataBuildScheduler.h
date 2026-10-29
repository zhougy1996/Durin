#pragma once

#include "CoreMinimal.h"
#include "DerivedDataCacheAPI.h"
#include "DerivedDataRequest.h"

namespace Durin::DerivedData
{
	enum class EBuildAdmissionReason : uint8
	{
		Closed, Capacity, MissingFunction, InvalidRequest, DispatchRejected, InternalFailure
	};
	struct FBuildAdmissionError
	{
		static constexpr size_t MaximumDescriptionBytes = 4096;
		EBuildAdmissionReason Reason = EBuildAdmissionReason::InvalidRequest;
		std::string Description;
		auto BoundDescription() -> void
		{ if (Description.size() > MaximumDescriptionBytes) Description.resize(MaximumDescriptionBytes); }
	};
	class IBuildScheduler;
	DERIVEDDATACACHE_API auto CreateInlineBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
	// Borrows the running Core task system; creates no threads or memory reservations.
	DERIVEDDATACACHE_API auto CreateTaskBuildScheduler() -> std::shared_ptr<IBuildScheduler>;
	DERIVEDDATACACHE_API auto CreateTaskCacheScheduler() -> std::shared_ptr<IBuildScheduler>;
}
