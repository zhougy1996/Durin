#pragma once

#include "AssetImportCore.h"
#include "ImportRecord.h"
#include "ImportRecordIndex.h"

namespace Durin::AssetImport
{
	enum class EMultiOutputObservedState : uint8
	{
		Absent,
		Managed,
		Referenced,
		Detached,
		Missing,
		Collision,
		Orphan
	};

	enum class EMultiOutputProposedAction : uint8
	{
		Create,
		ReplaceManaged,
		Reference,
		KeepDetached,
		ReportMissing,
		RejectCollision,
		ReportOrphan
	};

	struct FMultiOutputReconciliation
	{
		std::string StableIdentity;
		std::string Role;
		FAssetPath AssetPath;
		std::string AssetClassName;
		EImportRecordOutputPolicy PersistedPolicy = EImportRecordOutputPolicy::Managed;
		EMultiOutputObservedState ObservedState = EMultiOutputObservedState::Absent;
		EMultiOutputProposedAction ProposedAction = EMultiOutputProposedAction::Create;
		std::string PreviousAuthoredFingerprint;
		uint64 PackageEditRevision = 0;
		FAssetPath ObservedManager;

		auto operator==(const FMultiOutputReconciliation&) const -> bool = default;
	};

	struct FMultiOutputPlanRequest
	{
		FImportPlan GenericPlan;
		FAssetPath RecordPath;
		DImportRecord* ExistingRecord = nullptr;
		FImportRecordPayload ProviderState;
		FAssetPath PrimaryOutput;
		bool bRecreateMissingManagedOutputs = false;
		IImportProgressReporter* Progress = nullptr;
	};
	struct FMultiOutputPlanResult;
	struct FPreparedMultiOutputImport;
	struct FMultiOutputExecutionOptions;
	struct FMultiOutputExecutionResult;

	class ASSETIMPORTCORE_API FMultiOutputImportPlan
	{
	public:
		auto GetGenericPlan() const -> const FImportPlan& { return GenericPlan; }
		auto GetRecordPath() const -> const FAssetPath& { return RecordPath; }
		auto GetExistingRecord() const -> DImportRecord* { return ExistingRecord; }
		auto GetProviderState() const -> const FImportRecordPayload& { return ProviderState; }
		auto GetPrimaryOutput() const -> const FAssetPath& { return PrimaryOutput; }
		auto GetReconciliation() const -> std::span<const FMultiOutputReconciliation> { return Reconciliation; }
		auto GetOrphans() const -> std::span<const FMultiOutputReconciliation> { return Orphans; }
		auto GetPreview() const -> const FImportPreview& { return Preview; }
		auto GetIndexRevision() const -> uint64 { return IndexRevision; }

	private:
		FImportPlan GenericPlan;
		FAssetPath RecordPath;
		DImportRecord* ExistingRecord = nullptr;
		FImportRecordPayload ProviderState;
		FAssetPath PrimaryOutput;
		std::vector<FMultiOutputReconciliation> Reconciliation;
		std::vector<FMultiOutputReconciliation> Orphans;
		FImportPreview Preview;
		std::string ExistingRecordFingerprint;
		uint64 ExistingRecordEditRevision = 0;
		uint64 IndexRevision = 0;
		uint64 AssetRegistryRevision = 0;

		friend ASSETIMPORTCORE_API auto CreateMultiOutputImportPlan(
			const FMultiOutputPlanRequest&,
			FImportRecordIndex&) -> FMultiOutputPlanResult;
		friend ASSETIMPORTCORE_API auto ExecuteMultiOutputImport(
			const FMultiOutputImportPlan&,
			FPreparedMultiOutputImport,
			FImportRecordIndex&,
			const FMultiOutputExecutionOptions&) -> FMultiOutputExecutionResult;
	};

	struct FMultiOutputPlanResult
	{
		bool bSucceeded = false;
		std::string Message;
		FMultiOutputImportPlan Plan;
		std::vector<FImportDiagnostic> Diagnostics;
		explicit operator bool() const { return bSucceeded; }
	};

	struct FPreparedMultiOutput
	{
		std::string StableIdentity;
		DObject* ExistingTarget = nullptr;
		std::unique_ptr<ISingleAssetCandidate> Candidate;
		std::unique_ptr<IPreparedImportedStateExchange> Exchange;
	};

	struct FPreparedMultiOutputImport
	{
		FPreparedMultiOutputImport() = default;
		explicit FPreparedMultiOutputImport(FProviderLease InProvider)
			: Provider(std::move(InProvider)) {}
		ASSETIMPORTCORE_API ~FPreparedMultiOutputImport();
		FPreparedMultiOutputImport(FPreparedMultiOutputImport&&) noexcept = default;
		auto operator=(FPreparedMultiOutputImport&&) noexcept
			-> FPreparedMultiOutputImport& = delete;
		FPreparedMultiOutputImport(const FPreparedMultiOutputImport&) = delete;
		auto operator=(const FPreparedMultiOutputImport&)
			-> FPreparedMultiOutputImport& = delete;

		auto GetProvider() const -> const FProviderLease& { return Provider; }

		std::vector<FPreparedMultiOutput> Outputs;

	private:
		FProviderLease Provider;

		friend ASSETIMPORTCORE_API auto ExecuteMultiOutputImport(
			const FMultiOutputImportPlan&,
			FPreparedMultiOutputImport,
			FImportRecordIndex&,
			const FMultiOutputExecutionOptions&) -> FMultiOutputExecutionResult;
	};

	struct FMultiOutputExecutionOptions
	{
		Asset::FAssetBundleSaveOptions SaveOptions;
		IImportProgressReporter* Progress = nullptr;
		// Called only on the editor thread between candidate preparation and
		// validation boundaries. Publication ignores cancellation once started.
		std::function<bool()> IsCancellationRequested;
	};

	struct FMultiOutputExecutionResult
	{
		bool bSucceeded = false;
		std::string Message;
		DImportRecord* Record = nullptr;
		std::vector<DObject*> Outputs;
		std::vector<FAssetPath> Orphans;
		std::vector<FImportDiagnostic> Diagnostics;
		FProviderLease Provider;
		explicit operator bool() const { return bSucceeded; }
	};

	enum class EImportRecordAction : uint8
	{
		Reimport,
		RecreateMissingOutputs,
		RepairManagedOutputs
	};

	struct FImportRecordCapability
	{
		EImportRecordAction Action = EImportRecordAction::Reimport;
		bool bAvailable = false;
		std::string Label;
		std::vector<FImportDiagnostic> Diagnostics;
	};

	struct FImportRecordCapabilitySet
	{
		std::string ProviderId;
		std::vector<FImportRecordCapability> Capabilities;

		auto Find(EImportRecordAction Action) const -> const FImportRecordCapability*
		{
			const auto It = std::ranges::find(
				Capabilities, Action, &FImportRecordCapability::Action);
			return It == Capabilities.end() ? nullptr : &*It;
		}
	};

	struct FImportRecordActionResult
	{
		bool bSucceeded = false;
		std::string Message;
		DImportRecord* Record = nullptr;
		std::vector<DObject*> Outputs;
		std::vector<FAssetPath> Orphans;
		std::vector<FImportDiagnostic> Diagnostics;
		FProviderLease Provider;

		explicit operator bool() const { return bSucceeded; }
	};

	class ASSETIMPORTCORE_API IImportRecordHandler
	{
	public:
		virtual ~IImportRecordHandler() = default;
		virtual auto GetProviderId() const -> std::string_view = 0;
		virtual auto QueryCapabilities(
			const DImportRecord& Record,
			const FImportRecordInspection& Inspection) const
			-> FImportRecordCapabilitySet = 0;
		virtual auto Execute(
			DImportRecord& Record,
			EImportRecordAction Action,
			const FMultiOutputExecutionOptions& Options) const
			-> FImportRecordActionResult = 0;
	};

	class ASSETIMPORTCORE_API FImportRecordHandlerRegistry
	{
	public:
		FImportRecordHandlerRegistry();
		~FImportRecordHandlerRegistry();
		FImportRecordHandlerRegistry(const FImportRecordHandlerRegistry&) = delete;
		auto operator=(const FImportRecordHandlerRegistry&)
			-> FImportRecordHandlerRegistry& = delete;

		auto Register(std::shared_ptr<IImportRecordHandler> Handler,
			std::string& OutError) -> bool;
		auto Unregister(std::string_view ProviderId) -> bool;
		auto Find(std::string_view ProviderId) const
			-> std::shared_ptr<const IImportRecordHandler>;
		auto GetRevision() const -> uint64;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	ASSETIMPORTCORE_API auto CreateMultiOutputImportPlan(
		const FMultiOutputPlanRequest& Request,
		FImportRecordIndex& Index) -> FMultiOutputPlanResult;
	ASSETIMPORTCORE_API auto ExecuteMultiOutputImport(
		const FMultiOutputImportPlan& Plan,
		FPreparedMultiOutputImport Prepared,
		FImportRecordIndex& Index,
		const FMultiOutputExecutionOptions& Options = {}) -> FMultiOutputExecutionResult;
	ASSETIMPORTCORE_API auto BuildMultiOutputImportPreview(
		const FMultiOutputImportPlan& Plan) -> FImportPreview;
	ASSETIMPORTCORE_API auto GetImportRecordHandlerRegistry()
		-> FImportRecordHandlerRegistry&;
	ASSETIMPORTCORE_API auto QueryImportRecordCapabilities(
		const FImportRecordInspection& Inspection,
		FImportRecordHandlerRegistry& Handlers) -> FImportRecordCapabilitySet;
	ASSETIMPORTCORE_API auto ExecuteImportRecordAction(
		DImportRecord& Record,
		EImportRecordAction Action,
		FImportRecordHandlerRegistry& Handlers,
		const FMultiOutputExecutionOptions& Options = {}) -> FImportRecordActionResult;
}
