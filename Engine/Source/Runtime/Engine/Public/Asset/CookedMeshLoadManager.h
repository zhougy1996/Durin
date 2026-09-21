#pragma once

#include "Asset/BulkData.h"
#include "Asset/CookedMeshLoading.h"
#include "Asset/CookedMeshProducts.h"
#include "DObject/ObjectKey.h"
#include "EngineAPI.h"
#include "Threading/Task.h"

namespace Durin
{
	class DObject;

	enum class ECookedMeshFamily : uint8
	{
		StaticMesh = 0,
	};

	enum class ECookedMeshManagerState : uint8
	{
		Stopped,
		Accepting,
		Draining,
	};

	enum class ECookedMeshTerminalState : uint8
	{
		Succeeded,
		Failed,
		Cancelled,
		Stale,
		Rejected,
	};

	struct FCookedMeshLoadIdentity
	{
		FObjectKey Owner;
		ECookedMeshFamily Family = ECookedMeshFamily::StaticMesh;
		uint64 LoadGeneration = 0;
		uint64 ResourceRevision = 0;
		uint64 MetadataIdentity = 0;

		auto operator==(const FCookedMeshLoadIdentity& Other) const -> bool
		{
			return Owner == Other.Owner
				&& Family == Other.Family
				&& LoadGeneration == Other.LoadGeneration
				&& ResourceRevision == Other.ResourceRevision
				&& MetadataIdentity == Other.MetadataIdentity;
		}
	};

	enum class ECookedMeshAdmissionError : uint8
	{
		None, InvalidRequest, FieldSize, NotAccepting, StaleGeneration,
		IdentityConflict, PendingBudget, FlightBudget
	};
	struct FCookedMeshAdmissionError
	{
		ECookedMeshAdmissionError Code = ECookedMeshAdmissionError::None;
		FCookedMeshLoadIdentity Identity;
		ECookedMeshManagerState State = ECookedMeshManagerState::Stopped;
		uint64 FieldCount = 0;
		bool HasWorker = false;
		bool HasPublisher = false;
		uint64 Index = 0;
		uint64 FieldBytes = 0;
		uint64 RequestedBytes = 0;
		uint64 ReservedBytes = 0;
		uint64 ByteLimit = 0;
		uint64 RequestCount = 0;
		uint64 RequestLimit = 0;
		uint64 CurrentGeneration = 0;
		std::optional<FCookedMeshLoadIdentity> ExistingIdentity;
	};
	struct FCookedMeshAdmissionResult
	{
		FCookedMeshAdmissionError Error;
		explicit operator bool() const { return Error.Code == ECookedMeshAdmissionError::None; }
	};
	ENGINE_API auto FormatCookedMeshAdmissionError(const FCookedMeshAdmissionError& Error) -> std::string;

	class ENGINE_API ICookedMeshDetachedProduct
	{
	public:
		virtual ~ICookedMeshDetachedProduct() = default;
	};

	struct FCookedMeshWorkerResult
	{
		std::unique_ptr<ICookedMeshDetachedProduct> Product;
		FCookedMeshLoadError Error;
		uint64 RetainedBytes = 0;

		explicit operator bool() const { return Error.Code == ECookedMeshLoadError::None; }
	};

	using FCookedMeshWorker = std::move_only_function<FCookedMeshWorkerResult(
		std::span<const FSharedByteBuffer>, const FTaskCancellationToken&)>;
	using FCookedMeshCurrentPredicate = std::function<bool(
		const DObject&, const FCookedMeshLoadIdentity&)>;
	using FCookedMeshPublisher = std::move_only_function<FCookedMeshLoadResult(
		DObject&, const FCookedMeshLoadIdentity&,
		std::unique_ptr<ICookedMeshDetachedProduct>)>;
	using FCookedMeshTerminalCallback = std::move_only_function<void(
		DObject&, const FCookedMeshLoadIdentity&, ECookedMeshTerminalState,
		const FCookedMeshLoadError&)>;

	struct FCookedMeshLoadRequest
	{
		FCookedMeshLoadIdentity Identity;
		std::vector<FBulkData> Fields;
		FCookedMeshWorker Worker;
		FCookedMeshCurrentPredicate IsCurrent;
		FCookedMeshPublisher Publish;
		// Called on the GameThread for a still-current non-success terminal
		// result, including cancellation while shutdown drains without publication.
		FCookedMeshTerminalCallback OnTerminal;
	};

	struct FCookedMeshLoadManagerConfig
	{
		uint32 MaxConcurrentRequests = 8;
		uint64 MaxEstimatedBytes = 256ull * 1024ull * 1024ull;
		uint32 MaxPendingRequests = 8;
		uint64 MaxPendingEstimatedBytes = 256ull * 1024ull * 1024ull;
		uint64 MaxPendingCompletionBytes = 256ull * 1024ull * 1024ull;
		uint32 MaxIoPollsPerPump = 16;
		uint32 MaxCompletionsPerPump = 4;
	};

	struct FCookedMeshLoadDiagnostics
	{
		ECookedMeshManagerState State = ECookedMeshManagerState::Stopped;
		uint32 InFlightCount = 0;
		uint32 PendingRequestCount = 0;
		uint32 PendingCompletionCount = 0;
		uint64 InFlightEstimatedBytes = 0;
		uint64 PendingRequestEstimatedBytes = 0;
		uint64 PendingCompletionBytes = 0;
		uint32 PeakInFlightCount = 0;
		uint32 PeakPendingRequestCount = 0;
		uint32 PeakPendingCompletionCount = 0;
		uint64 PeakInFlightEstimatedBytes = 0;
		uint64 PeakPendingRequestEstimatedBytes = 0;
		uint64 PeakPendingCompletionBytes = 0;
		uint64 ReadReadyMicroseconds = 0;
		uint64 WorkerMicroseconds = 0;
		uint64 GameThreadCompletionMicroseconds = 0;
		uint64 AcceptedCount = 0;
		uint64 CoalescedCount = 0;
		uint64 SupersededCount = 0;
		uint64 RejectedCount = 0;
		uint64 SucceededCount = 0;
		uint64 FailedCount = 0;
		uint64 CancelledCount = 0;
		uint64 StaleCount = 0;
	};

	class ENGINE_API FCookedMeshLoadManager
	{
	public:
		explicit FCookedMeshLoadManager(FCookedMeshLoadManagerConfig Config = {});
		~FCookedMeshLoadManager();
		FCookedMeshLoadManager(const FCookedMeshLoadManager&) = delete;
		auto operator=(const FCookedMeshLoadManager&)
			-> FCookedMeshLoadManager& = delete;

		// All lifecycle and request mutations are GameThread-only. Submit starts
		// asynchronous reads only after count and byte admission succeeds.
		auto Initialize() -> bool;
		auto Submit(FCookedMeshLoadRequest Request) -> FCookedMeshAdmissionResult;
		// Polls ready I/O, launches worker work, and publishes a bounded number
		// of current results without waiting for unfinished package requests.
		auto Pump() -> uint32;
		auto Cancel(FObjectKey Owner) -> bool;
		// Explicit blocking compatibility boundary for one selected owner.
		auto Finish(FObjectKey Owner) -> void;
		auto StopAdmission() -> void;
		// Cancels and drains every owned read/task/result without publication.
		auto Shutdown() -> void;
		auto GetDiagnostics() const -> FCookedMeshLoadDiagnostics;

	private:
		struct FState;
		std::shared_ptr<FState> State;
	};

	ENGINE_API auto InitializeCookedMeshLoadManager() -> bool;
	ENGINE_API auto PumpCookedMeshLoadManager() -> uint32;
	ENGINE_API auto ShutdownCookedMeshLoadManager() -> void;
	ENGINE_API auto GetCookedMeshLoadManager() -> FCookedMeshLoadManager*;
}
