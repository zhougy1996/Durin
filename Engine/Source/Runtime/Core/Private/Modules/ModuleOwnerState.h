#pragma once

#include "Misc/Name.h"

namespace Durin::Detail
{
	struct FAsyncOperationGroupState;

	// Carries immutable load-generation identity plus independently retired feature and operation admission.
	struct FModularFeatureOwnerState
	{
		FName Name;
		uint64 Generation = 0;
		std::atomic<bool> bFeatureAdmissionRetired = false;
		std::atomic<bool> bOperationAdmissionRetired = false;
		std::mutex OperationMutex;
		std::vector<std::shared_ptr<FAsyncOperationGroupState>> OperationGroups;
	};
}
