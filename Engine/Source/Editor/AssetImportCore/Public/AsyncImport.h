#pragma once

#include "AssetImportCore.h"

namespace Durin::AssetImport
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

		friend ASSETIMPORTCORE_API auto LaunchAsyncImportPlan(
			FImportPlanRequest, std::string_view) -> FAsyncImportPlanHandle;
		friend ASSETIMPORTCORE_API auto TryTakeAsyncImportPlanResult(
			const FAsyncImportPlanHandle&, FImportPlanResult&) -> EAsyncImportPlanStatus;
		friend ASSETIMPORTCORE_API auto CancelAsyncImport(
			const FAsyncImportPlanHandle&) -> bool;
		friend ASSETIMPORTCORE_API auto CancelAndDrainAsyncImport(
			const FAsyncImportPlanHandle&) -> EAsyncImportPlanStatus;
		friend class FAsyncImportCoordinator;
	};

	// Starts one framework-owned preparation task. OwnerId identifies the
	// single-consumer request slot (for example one import dialog). A newer
	// request with the same owner supersedes and cancels the older request.
	ASSETIMPORTCORE_API auto LaunchAsyncImportPlan(
		FImportPlanRequest Request,
		std::string_view OwnerId) -> FAsyncImportPlanHandle;

	// Drains value-only completion notices on the editor thread. Results and
	// provider leases remain in request state until taken or discarded.
	ASSETIMPORTCORE_API auto DrainAsyncImportCompletionMailbox() -> uint64;
	ASSETIMPORTCORE_API auto TryTakeAsyncImportPlanResult(
		const FAsyncImportPlanHandle& Handle,
		FImportPlanResult& OutResult) -> EAsyncImportPlanStatus;

	ASSETIMPORTCORE_API auto CancelAsyncImport(
		const FAsyncImportPlanHandle& Handle) -> bool;
	ASSETIMPORTCORE_API auto CancelAndDrainAsyncImport(
		const FAsyncImportPlanHandle& Handle) -> EAsyncImportPlanStatus;
	ASSETIMPORTCORE_API auto CancelAndDrainAsyncImportsForOwner(
		std::string_view OwnerId) -> void;
	ASSETIMPORTCORE_API auto CancelAndDrainAsyncImportsForProvider(
		std::string_view ProviderId) -> void;
	ASSETIMPORTCORE_API auto CancelAndDrainAllAsyncImports() -> void;

	// Provider admission is closed before module-owned handlers are unregistered.
	// Registration may reopen the provider after its module has started again.
	ASSETIMPORTCORE_API auto OpenAsyncImportProviderAdmission(
		std::string_view ProviderId) -> void;
	ASSETIMPORTCORE_API auto CloseAsyncImportProviderAdmission(
		std::string_view ProviderId) -> void;
	ASSETIMPORTCORE_API auto CloseAsyncImportAdmission() -> void;

	// Synchronous provider code can poll this at bounded CPU phase boundaries.
	// It returns false outside a coordinator-owned worker task.
	ASSETIMPORTCORE_API auto IsImportCancellationRequested() -> bool;
}
