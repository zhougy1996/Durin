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
		uint32 MaxCompletionsPerPump = 4;
	};

	struct FCookedMeshLoadDiagnostics
	{
		uint32 InFlightCount = 0;
		uint32 PendingRequestCount = 0;
		uint32 PeakInFlightCount = 0;
		uint64 AcceptedCount = 0;
		uint64 SucceededCount = 0;
		uint64 FailedCount = 0;
		uint64 CancelledCount = 0;
	};

	// GameThread scheduling limits concurrent loads and publications per pump.
	// Updates own their asset association, execution state, inputs and results.
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
		FCookedMeshLoadDiagnostics Diagnostics;
	};

	ENGINE_API auto GetCookedMeshLoadManager() -> FCookedMeshLoadManager*;
}
