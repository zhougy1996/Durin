#pragma once

#include "AssetForge/ImportRequest.h"
#include "AssetForge/ImportResult.h"
#include "AssetForge/Extensions/ComponentRegistration.h"
#include "AssetForge/Operations/ImportOperation.h"
#include "AssetForge/Operations/ImportJob.h"
#include "AssetForge/Operations/ImportExecution.h"

namespace Durin::AssetForge
{
	class ASSETFORGE_API FImportService final
	{
	public:
		FImportService();
		~FImportService();
		FImportService(const FImportService&) = delete;
		auto operator=(const FImportService&) -> FImportService& = delete;

		auto RegisterSourceTranslatorScoped(FSourceTranslatorRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FComponentRegistration;
		auto RegisterPlanningPassScoped(FPlanningPassRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FComponentRegistration;
		auto RegisterAssetBuilderScoped(FAssetBuilderRegistrationDescriptor Descriptor,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError)
			-> FComponentRegistration;
		auto FindComponent(EComponentRole Role,
			std::string_view Id, uint32 ContractVersion = 0) const
			-> FComponentLease;
		auto SelectSourceTranslator(const FImportSourceRecognition& Source,
			std::string_view PersistedId = {}, uint32 PersistedVersion = 0) const
			-> FComponentSelectionResult;
		auto SelectAssetBuilder(std::string_view OutputClassName,
			std::string_view PersistedId = {}, uint32 PersistedVersion = 0) const
			-> FComponentSelectionResult;
		auto EnumerateComponents(EComponentRole Role) const
			-> std::vector<FComponentIdentity>;
		auto GetComponentRevision() const -> uint64;
		auto SubmitImport(FImportRequest Request,
			std::string_view Title = {},
			FImportCompletion Completion = {}) -> FImportHandle;
		auto RunImportInline(FImportRequest Request,
			std::string_view Title = {}) -> FImportResult;
		auto SubmitImportJob(std::unique_ptr<IImportJob> Job,
			std::string_view Title) -> FImportOperationHandle;
		// Called once from the central editor tick. It never waits for worker work.
		auto PumpImportOperations(uint32 MaximumEditorSteps = 64) -> uint32;
		auto CancelAndDrainImportOperation(
			const FImportOperationHandle& Handle) -> void;
		auto RunImportJobInline(std::unique_ptr<IImportJob> Job,
			std::string_view Title = {}) -> FImportOutcome;
		auto CancelImportOperation(const FImportOperationHandle& Handle) -> bool;
		auto HasActiveImportClaim(std::string_view Identity) const -> bool;
		auto ReleaseImportPreviewOwner(std::string_view OwnerId) -> void;
		auto CancelAndDrainAsyncImportsForOwner(std::string_view OwnerId) -> void;
		auto CancelAndDrainAsyncImportsForProvider(std::string_view ProviderId) -> void;
		auto CancelAndDrainAllAsyncImports() -> void;
		auto CloseAsyncAdmission() -> void;
	private:
		friend class FComponentRegistration;
		struct FImpl;
		std::shared_ptr<void> Lifetime;
		std::unique_ptr<FImpl> Impl;
		auto OpenAsyncImporterAdmission(std::string_view ProviderId) -> void;
		auto UnregisterComponent(EComponentRole Role,
			std::string_view Id, uint64 Identity) -> bool;


	};

	ASSETFORGE_API auto GetImportService() -> FImportService&;
}
