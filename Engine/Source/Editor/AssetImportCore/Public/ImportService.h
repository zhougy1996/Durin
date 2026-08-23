#pragma once

#include "AssetImportCore.h"
#include "AsyncImport.h"
#include "ImportJob.h"
#include "MultiOutputImport.h"

namespace Durin::Asset
{
	class FImportService;
	struct FSingleAssetAsyncExecutionState;
	struct FSingleAssetAsyncPlanState;

	class ASSETIMPORTCORE_API FSingleAssetAsyncPlanHandle
	{
	public:
		FSingleAssetAsyncPlanHandle() = default;
		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }
	private:
		explicit FSingleAssetAsyncPlanHandle(
			std::shared_ptr<FSingleAssetAsyncPlanState> InState)
			: State(std::move(InState)) {}
		std::shared_ptr<FSingleAssetAsyncPlanState> State;
		friend class FImportService;
	};

	class ASSETIMPORTCORE_API FSingleAssetAsyncExecutionHandle
	{
	public:
		FSingleAssetAsyncExecutionHandle() = default;
		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }

	private:
		explicit FSingleAssetAsyncExecutionHandle(
			std::shared_ptr<FSingleAssetAsyncExecutionState> InState)
			: State(std::move(InState)) {}
		std::shared_ptr<FSingleAssetAsyncExecutionState> State;
		friend class FImportService;
	};

	class ASSETIMPORTCORE_API FImporterRegistration final
	{
	public:
		FImporterRegistration() = default;
		~FImporterRegistration();
		FImporterRegistration(const FImporterRegistration&) = delete;
		auto operator=(const FImporterRegistration&) -> FImporterRegistration& = delete;
		FImporterRegistration(FImporterRegistration&& Other) noexcept;
		auto operator=(FImporterRegistration&& Other) noexcept
			-> FImporterRegistration&;

		explicit operator bool() const { return Owner != nullptr; }
		auto Reset() -> bool;

	private:
		friend class FImportService;
		FImporterRegistration(FImportService& InOwner,
			std::weak_ptr<void> InOwnerLifetime, std::string InProviderId,
			uint64 InIdentity);

		FImportService* Owner = nullptr;
		std::weak_ptr<void> OwnerLifetime;
		std::string ProviderId;
		uint64 Identity = 0;
	};

	struct FImporterDescriptor
	{
		std::string ProviderId;
		uint32 ContractVersion = 0;
		std::vector<std::string> SourceExtensions;
		std::shared_ptr<IImportProvider> Provider;
		std::vector<std::shared_ptr<ISingleAssetImportHandler>> SingleAssetHandlers;
		std::shared_ptr<IImportRecordHandler> RecordHandler;
	};

	class ASSETIMPORTCORE_API FImportService final
	{
	public:
		FImportService();
		~FImportService();
		FImportService(const FImportService&) = delete;
		auto operator=(const FImportService&) -> FImportService& = delete;

		auto RegisterImporter(FImporterDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError) -> bool;
		auto RegisterImporterScoped(FImporterDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FImporterRegistration;
		auto UnregisterImporter(std::string_view ProviderId) -> bool;
		auto IsImporterRegistered(std::string_view ProviderId) const -> bool;
		auto FindImporter(std::string_view ProviderId) const -> FProviderLease;
		auto GetOutstandingImporterLeaseCount(std::string_view ProviderId) const -> uint64;
		auto GetImporterRevision() const -> uint64;
		auto SubmitImportJob(std::unique_ptr<IImportJob> Job,
			std::string_view Title) -> FImportOperationHandle;
		// Called once from the central editor tick. It never waits for worker work.
		auto PumpImportOperations(uint32 MaximumEditorSteps = 64) -> uint32;
		auto RunImportJobInline(std::unique_ptr<IImportJob> Job,
			std::string_view Title = {}) -> FImportOutcome;
		auto CancelImportOperation(const FImportOperationHandle& Handle) -> bool;
		auto HasActiveImportClaim(std::string_view Identity) const -> bool;
		auto ReleaseImportPreviewOwner(std::string_view OwnerId) -> void;
		auto LaunchAsyncImportPlan(FImportPlanRequest Request,
			std::string_view OwnerId,
			bool bKeepOperationOpenAfterPlan = false) -> FAsyncImportPlanHandle;
		auto BeginAsyncImportOperation(
			std::string_view OwnerId,
			std::string_view ProviderId,
			std::string_view Title) -> FAsyncImportPlanHandle;
		auto CancelAsyncImport(const FAsyncImportPlanHandle& Handle) -> bool;
		auto CancelAndDrainAsyncImport(const FAsyncImportPlanHandle& Handle)
			-> EAsyncImportPlanStatus;
		auto CancelAndDrainAsyncImportsForOwner(std::string_view OwnerId) -> void;
		auto CancelAndDrainAsyncImportsForProvider(std::string_view ProviderId) -> void;
		auto CancelAndDrainAllAsyncImports() -> void;
		auto CloseAsyncAdmission() -> void;
		auto CreateImportPlan(const FImportPlanRequest& Request) -> FImportPlanResult;
		auto HasSingleAssetImporter(std::string_view AssetClassName) const -> bool;
		auto QuerySingleAssetCapabilities(const DObject& Asset) const
			-> FSingleAssetCapabilitySet;
		auto CreateSingleAssetReimportPlan(const FSingleAssetReimportRequest& Request)
			-> FSingleAssetPlanResult;
		auto BeginSingleAssetReimportPlan(
			const FSingleAssetReimportRequest& Request,
			FSingleAssetAsyncPlanOptions Options = {},
			FTaskScopeToken OperationScope = {}) -> FSingleAssetAsyncPlanHandle;
		auto PollSingleAssetReimportPlan(
			FSingleAssetAsyncPlanHandle& Handle,
			FSingleAssetPlanResult& OutResult) -> EAsyncImportPlanStatus;
		auto CancelAndDrainSingleAssetReimportPlan(
			FSingleAssetAsyncPlanHandle& Handle) -> void;
		auto ExecuteSingleAssetImport(const FSingleAssetImportPlan& Plan,
			const FSingleAssetExecutionOptions& Options = {})
			-> FSingleAssetExecutionResult;
		auto BeginSingleAssetImportExecution(
			const FSingleAssetImportPlan& Plan,
			FSingleAssetExecutionOptions Options = {},
			FTaskScopeToken OperationScope = {})
			-> FSingleAssetAsyncExecutionHandle;
		auto PollSingleAssetImportExecution(
			FSingleAssetAsyncExecutionHandle& Handle,
			FSingleAssetExecutionResult& OutResult) -> EAsyncImportPlanStatus;
		auto CancelAndDrainSingleAssetImportExecution(
			FSingleAssetAsyncExecutionHandle& Handle) -> void;
		auto RepairSingleAssetSource(DObject& Asset,
			std::span<const FSourcePath> Sources) -> FSingleAssetExecutionResult;
		auto CreateMultiOutputImportPlan(const FMultiOutputPlanRequest& Request,
			FImportRecordIndex& Index) -> FMultiOutputPlanResult;
		auto ExecuteMultiOutputImport(const FMultiOutputImportPlan& Plan,
			FPreparedMultiOutputImport Prepared, FImportRecordIndex& Index,
			const FMultiOutputExecutionOptions& Options = {})
			-> FMultiOutputExecutionResult;
		auto QueryImportRecordCapabilities(const FImportRecordInspection& Inspection) const
			-> FImportRecordCapabilitySet;
		auto ExecuteImportRecordAction(DImportRecord& Record,
			EImportRecordAction Action,
			const FMultiOutputExecutionOptions& Options = {})
			-> FImportRecordActionResult;
		auto GetRevision() const -> uint64;

	private:
		friend class FImporterRegistration;
		struct FImpl;
		std::shared_ptr<void> Lifetime;
		std::unique_ptr<FImpl> Impl;
		auto FindProvider(std::string_view ProviderId) const -> FProviderLease;
		auto FindSingleAssetHandler(std::string_view AssetClassName) const
			-> std::shared_ptr<const ISingleAssetImportHandler>;
		auto FindImportRecordHandler(std::string_view ProviderId) const
			-> std::shared_ptr<const IImportRecordHandler>;
		auto OpenAsyncImporterAdmission(std::string_view ProviderId) -> void;
		auto UnregisterImporter(std::string_view ProviderId, uint64 Identity) -> bool;
		auto ExecuteSingleAssetImportPrepared(
			const FSingleAssetImportPlan& Plan,
			const FSingleAssetExecutionOptions& Options,
			std::unique_ptr<ISingleAssetPreparedProduct> Prepared,
			std::vector<FImportDiagnostic> PreparationDiagnostics)
			-> FSingleAssetExecutionResult;

	};

	ASSETIMPORTCORE_API auto GetImportService() -> FImportService&;
}
