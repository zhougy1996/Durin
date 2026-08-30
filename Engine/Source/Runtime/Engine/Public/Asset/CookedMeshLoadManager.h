#pragma once

#include "Asset/BulkData.h"
#include "DObject/ObjectHandle.h"
#include "EngineAPI.h"
#include "Templates/MoveOnlyFunction.h"
#include "Threading/Task.h"

namespace Durin
{
	class DObject;

	namespace Asset
	{
		enum class ECookedMeshFamily : uint8
		{
			StaticMesh,
			SkeletalMesh,
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
			FObjectHandle Owner;
			ECookedMeshFamily Family = ECookedMeshFamily::StaticMesh;
			uint64 LoadGeneration = 0;
			uint64 ResourceRevision = 0;
			uint64 MetadataIdentity = 0;

			auto operator==(const FCookedMeshLoadIdentity& Other) const -> bool
			{
				return Owner.Index == Other.Owner.Index
					&& Owner.Generation == Other.Owner.Generation
					&& Family == Other.Family
					&& LoadGeneration == Other.LoadGeneration
					&& ResourceRevision == Other.ResourceRevision
					&& MetadataIdentity == Other.MetadataIdentity;
			}
		};

		class ENGINE_API ICookedMeshDetachedProduct
		{
		public:
			virtual ~ICookedMeshDetachedProduct() = default;
		};

		struct FCookedMeshWorkerResult
		{
			std::unique_ptr<ICookedMeshDetachedProduct> Product;
			std::string Message;
			uint64 RetainedBytes = 0;

			explicit operator bool() const { return Product != nullptr; }
		};

		using FCookedMeshWorker = Durin::Private::TMoveOnlyFunction<FCookedMeshWorkerResult(
			std::span<const FSharedByteBuffer>, const FTaskCancellationToken&)>;
		using FCookedMeshCurrentPredicate = std::function<bool(
			const DObject&, const FCookedMeshLoadIdentity&)>;
		using FCookedMeshPublisher = Durin::Private::TMoveOnlyFunction<bool(
			DObject&, const FCookedMeshLoadIdentity&,
			std::unique_ptr<ICookedMeshDetachedProduct>, std::string&)>;
		using FCookedMeshTerminalCallback = Durin::Private::TMoveOnlyFunction<void(
			DObject&, const FCookedMeshLoadIdentity&, ECookedMeshTerminalState,
			std::string_view)>;

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
			auto Submit(FCookedMeshLoadRequest Request) -> bool;
			// Polls ready I/O, launches worker work, and publishes a bounded number
			// of current results without waiting for unfinished package requests.
			auto Pump() -> uint32;
			auto Cancel(FObjectHandle Owner) -> bool;
			// Explicit blocking compatibility boundary for one selected owner.
			auto Finish(FObjectHandle Owner) -> void;
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
}
