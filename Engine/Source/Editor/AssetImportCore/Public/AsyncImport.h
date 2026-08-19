#pragma once

#include "AssetImportCore.h"

namespace Durin::Asset
{
	struct FAsyncImportRequestState;
	class FAsyncImportCoordinator;

	enum class EAsyncImportPlanStatus : uint8
	{
		Invalid,
		Pending,
		Succeeded,
		Failed,
		Canceled,
		Superseded,
		Rejected
	};

	// Copyable observation handle. The coordinator owns accepted work; retaining
	// a handle does not retain a provider lease after its result is consumed.
	class ASSETIMPORTCORE_API FAsyncImportPlanHandle
	{
	public:
		FAsyncImportPlanHandle() = default;

		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetSerial() const -> uint64;
		auto GetStatus() const -> EAsyncImportPlanStatus;

	private:
		explicit FAsyncImportPlanHandle(
			std::shared_ptr<FAsyncImportRequestState> InState)
			: State(std::move(InState)) {}

		std::shared_ptr<FAsyncImportRequestState> State;

		friend class FImportService;
		friend ASSETIMPORTCORE_API auto TryTakeAsyncImportPlanResult(
			const FAsyncImportPlanHandle&, FImportPlanResult&) -> EAsyncImportPlanStatus;
		friend class FAsyncImportCoordinator;
	};

	// Drains value-only completion notices on the editor thread. Results and
	// provider leases remain in request state until taken or discarded.
	ASSETIMPORTCORE_API auto DrainAsyncImportCompletionMailbox() -> uint64;
	ASSETIMPORTCORE_API auto TryTakeAsyncImportPlanResult(
		const FAsyncImportPlanHandle& Handle,
		FImportPlanResult& OutResult) -> EAsyncImportPlanStatus;

	// Synchronous provider code can poll this at bounded CPU phase boundaries.
	// It returns false outside a coordinator-owned worker task.
	ASSETIMPORTCORE_API auto IsImportCancellationRequested() -> bool;
}
