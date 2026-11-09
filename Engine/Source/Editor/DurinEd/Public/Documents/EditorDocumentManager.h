#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Asset/MutationExtensions.h"

namespace Durin { class DAssetEditorSubsystem; class IPackageReloadParticipant; }

namespace Durin
{
	namespace DocumentPrivate { struct FEditorDocumentState; }

	// Owns one session's document identity, activation, persistence and close decisions.
	// Tab presentation and docking belong to FTabManager and TabLayout.
	// Every resolved asset document retains its asset until closed or its controller is destroyed.
	class FEditorDocumentManager final : private IAssetMoveObserver
	{
	public:
		DURINED_API FEditorDocumentManager();
		DURINED_API ~FEditorDocumentManager();
		FEditorDocumentManager(const FEditorDocumentManager&) = delete;
		auto operator=(const FEditorDocumentManager&) -> FEditorDocumentManager& = delete;

		DURINED_API auto AttachSessionToolkit(std::shared_ptr<IAssetEditorToolkit> Toolkit,
			std::function<void()> AssetsChanged, DAssetEditorSubsystem* Service = nullptr) -> bool;
		// Publishes an already activated scene without re-entering the loading operation.
		DURINED_API auto AssociateDocumentAsset(FDocumentId Id, DObject* Asset) -> bool;
		DURINED_API auto GetEditedAssets() const -> std::vector<DObject*>;
		DURINED_API auto GetAssetEditorSubsystem() const -> DAssetEditorSubsystem*;
		DURINED_API auto OpenDocument(FDocumentRequest Request) -> FDocumentId;
		DURINED_API auto CompleteDeferredDocumentOpen(FDocumentId DocumentId, bool bSucceeded,
			EAssetOpenStatus FailureStatus = EAssetOpenStatus::Failed, std::string Message = {}) -> bool;
		DURINED_API auto IsDocumentOpenPending(FDocumentId DocumentId) const -> bool;
		DURINED_API auto ActivateDocument(FDocumentId DocumentId) -> bool;
		DURINED_API auto RequestDeactivateActiveToolkit() -> bool;
		DURINED_API auto RequestCloseDocument(FDocumentId DocumentId) -> EDocumentCloseResult;
		// Applies one response to the single pending close without losing it on save or discard failure.
		DURINED_API auto ResolvePendingDocumentClose(EDocumentCloseResponse Response) -> EDocumentCloseResult;
		DURINED_API auto RefreshDocumentState() -> void;
		DURINED_API auto NotifyPackageReloaded(DPackage* Previous, DPackage* Replacement) -> void;
		// Migrates this manager's open-document strong references during atomic reload.
		// Call NotifyPackageReloaded after publication to refresh editor UI and indexing.
		DURINED_API auto CreateDocumentReloadParticipant() -> std::shared_ptr<IPackageReloadParticipant>;
		// Settles previews and refreshes all documents without closing tabs. Pending operations veto exit.
		DURINED_API auto PrepareForExit() -> bool;
		// Saves every dirty document, including non-closable tabs; failure keeps all tabs open.
		DURINED_API auto SaveDocumentsForExit() -> bool;
		// Updates open and deferred document routing after an authoritative asset move.
		DURINED_API auto RemapResourceId(
			std::string_view SourceResourceId,
			std::string_view DestinationResourceId) -> void;

		DURINED_API auto GetDocuments() const -> const std::vector<FEditorDocument>&;
		DURINED_API auto GetActiveDocument() -> FEditorDocument*;
		DURINED_API auto GetActiveDocument() const -> const FEditorDocument*;
		DURINED_API auto GetPendingCloseDocument() const -> const FEditorDocument*;
		DURINED_API auto GetPendingCloseSequence() const -> uint64;
		DURINED_API auto GetCloseCancellationRevision() const -> uint64;
		DURINED_API auto FindToolkit(const FAssetEditorToolkitId& ToolkitId) const -> std::shared_ptr<IAssetEditorToolkit>;
		DURINED_API auto GetToolkit() const -> std::shared_ptr<IAssetEditorToolkit>;

	private:
		auto SynchronizeAssets() -> void;
		auto OnAssetsRelocated(
			std::span<const FAssetRelocationMapping> Mappings) -> void override;
		auto FindDocument(FDocumentId DocumentId) -> FEditorDocument*;
		auto FindDocument(const FAssetEditorToolkitId& ToolkitId, std::string_view DocumentKey) -> FEditorDocument*;
		auto RequestDeactivateActiveDocument(FDocumentId NextDocumentId = {}) -> bool;

		static auto AssetLabel(std::string_view ResourceId) -> std::string;

		std::shared_ptr<DocumentPrivate::FEditorDocumentState> State;
		FAssetMoveObserverHandle AssetMoveObserverHandle = 0;
	};
}
