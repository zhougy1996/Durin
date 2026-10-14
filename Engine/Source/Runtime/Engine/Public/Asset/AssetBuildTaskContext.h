#pragma once

#include "CoreMinimal.h"
#include "Misc/CoreTypes.h"
#include "Threading/CancellationToken.h"

namespace Durin
{
	inline constexpr size_t MaximumAssetBuildDiagnosticBytes = 4096;
	inline constexpr uint64 MaximumAssetBuildRequestBytes = 512ull * 1024 * 1024;
	// Ordinary failures carry bounded owned text; cancellation stays in the token.
	inline auto MakeAssetBuildDiagnostic(std::string_view Message, std::string_view Stage = {}) -> std::string
	{
		const auto PrefixBytes = Stage.empty() ? size_t{0} : std::min(Stage.size(), MaximumAssetBuildDiagnosticBytes - 2) + 2;
		const auto MessageBytes = std::min(Message.size(), MaximumAssetBuildDiagnosticBytes - PrefixBytes);
		std::string Diagnostic;
		Diagnostic.reserve(PrefixBytes + MessageBytes);
		if (PrefixBytes)
		{
			Diagnostic.append(Stage.substr(0, PrefixBytes - 2));
			Diagnostic.append(": ");
		}
		Diagnostic.append(Message.substr(0, MessageBytes));
		return Diagnostic;
	}

	// Worker-local observations; no concurrent access is permitted during an invocation.
	struct FAssetBuildTaskMetrics
	{
		uint64 CancellationCheckpoints = 0;
	};

	// Cancellation is shared across the invocation. Metrics are borrowed worker-local data.
	struct FAssetBuildTaskContext
	{
		FCancellationToken Cancellation;
		FAssetBuildTaskMetrics* Metrics = nullptr;
		// Construction envelope. Providers must reject expansion before allocating scratch/products.
		uint64 MaximumWorkingSetBytes = std::numeric_limits<uint64>::max();

		auto IsCancelled() const -> bool
		{
			if (Metrics) ++Metrics->CancellationCheckpoints;
			return Cancellation.IsCancelled();
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
