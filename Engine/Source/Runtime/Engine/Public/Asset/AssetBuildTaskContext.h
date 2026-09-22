#pragma once

#include <functional>
#include <limits>
#include "Misc/CoreTypes.h"

namespace Durin
{
	// Worker-local observations; no concurrent access is permitted during an invocation.
	struct FAssetBuildTaskMetrics
	{
		uint64 CancellationCheckpoints = 0;
	};

	// Borrowed invocation controls. Neither provider nor product may retain these values.
	struct FAssetBuildTaskContext
	{
		std::function<bool()> ShouldCancel;
		FAssetBuildTaskMetrics* Metrics = nullptr;
		// Whole-operation reservation. Providers must reject expansion before allocating scratch/products.
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();

		auto IsCancelled() const -> bool
		{
			if (Metrics) ++Metrics->CancellationCheckpoints;
			return ShouldCancel && ShouldCancel();
		}
	};

	// Checked conservative allocation envelope used before recipe and acceleration construction.
	struct FAssetBuildMemoryEstimate
	{
		uint64 Limit;
		uint64 Bytes = 0;
		uint64 RejectedCount = 0;
		uint64 RejectedWidth = 0;
		auto Add(uint64 Count, uint64 Width) -> bool
		{
			if (Width == 0 || Count > (Limit - Bytes) / Width)
			{
				RejectedCount = Count;
				RejectedWidth = Width;
				return false;
			}
			Bytes += Count * Width;
			return true;
		}
	};


}
