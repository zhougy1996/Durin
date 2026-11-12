#pragma once

#include "CoreMinimal.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Threading/TaskRuntime.h"

namespace Durin
{
	class FStaticMeshLoadUpdate;

	struct FCookedMeshLoadManagerConfig
	{
		uint32 MaxConcurrentRequests = 8;
		uint64 MaxEstimatedBytes = 256ull * 1024ull * 1024ull;
		uint64 MaxPendingCompletionBytes = 256ull * 1024ull * 1024ull;
		uint32 MaxIoPollsPerPump = 16;
		uint32 MaxCompletionsPerPump = 4;
	};

	struct FCookedMeshLoadDiagnostics
	{
		uint32 InFlightCount = 0;
		uint32 PendingRequestCount = 0;
		uint32 PendingCompletionCount = 0;
		uint64 InFlightEstimatedBytes = 0;
		uint64 PendingCompletionBytes = 0;
		uint32 PeakInFlightCount = 0;
		uint64 PeakInFlightEstimatedBytes = 0;
		uint64 AcceptedCount = 0;
		uint64 SucceededCount = 0;
		uint64 FailedCount = 0;
		uint64 CancelledCount = 0;
	};

	// GameThread scheduler. Updates own their asset association, inputs and results.
	class ENGINE_API FCookedMeshLoadManager
	{
	public:
		explicit FCookedMeshLoadManager(FCookedMeshLoadManagerConfig InConfig = {});
		~FCookedMeshLoadManager();
		FCookedMeshLoadManager(const FCookedMeshLoadManager&) = delete;
		auto operator=(const FCookedMeshLoadManager&) -> FCookedMeshLoadManager& = delete;
		auto Initialize() -> bool;
		auto Submit(std::shared_ptr<FStaticMeshLoadUpdate> Update) -> bool;
		auto Pump() -> uint32;
		auto Shutdown() -> void;
		auto GetDiagnostics() const -> FCookedMeshLoadDiagnostics;

	private:
		auto RecordCompletion(const FStaticMeshLoadUpdate& Update) -> void;
		FCookedMeshLoadManagerConfig Config;
		bool bAccepting = false;
		FTaskAttribution Attribution;
		std::vector<std::shared_ptr<FStaticMeshLoadUpdate>> Pending;
		std::vector<std::shared_ptr<FStaticMeshLoadUpdate>> Active;
		uint64 InFlightEstimatedBytes = 0;
		FCookedMeshLoadDiagnostics Diagnostics;
	};

	ENGINE_API auto GetCookedMeshLoadManager() -> FCookedMeshLoadManager*;
}
