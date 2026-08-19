#pragma once

#include "AssetImportCore.h"
#include "AsyncImport.h"
#include "MultiOutputImport.h"

namespace Durin::Asset::Import
{
	class FImportService;

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
		auto LaunchAsyncImportPlan(FImportPlanRequest Request,
			std::string_view OwnerId) -> FAsyncImportPlanHandle;
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
		auto ExecuteSingleAssetImport(const FSingleAssetImportPlan& Plan,
			const FSingleAssetExecutionOptions& Options = {})
			-> FSingleAssetExecutionResult;
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

	};

	ASSETIMPORTCORE_API auto GetImportService() -> FImportService&;
}
