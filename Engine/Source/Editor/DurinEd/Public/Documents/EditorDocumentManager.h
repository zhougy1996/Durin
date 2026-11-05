#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Asset/MutationExtensions.h"

namespace Durin { class DAssetEditorSubsystem; class IPackageReloadParticipant; }

namespace Durin::Editor
{
	// Couples a toolkit implementation with its host descriptor.
	struct FAssetEditorToolkitRegistration
	{
		FAssetEditorToolkitDescriptor Descriptor;
		std::shared_ptr<IAssetEditorToolkit> Toolkit;
	};

	// Registers a dependency-consistent group of toolkits and asset editors.
	struct FAssetEditorToolkitRegistrationBatch
	{
		std::vector<FAssetEditorToolkitRegistration> Toolkits;
		std::vector<FAssetEditorRegistration> AssetEditors;
	};

	namespace Detail
	{
		// Shared registry storage kept alive independently from registration handles.
		struct FEditorDocumentRegistryState;
	}

	// Owns one registry lease and unregisters its batch when reset or destroyed.
	class FAssetEditorToolkitRegistrationHandle
	{
	public:
		FAssetEditorToolkitRegistrationHandle() = default;
		DURINED_API ~FAssetEditorToolkitRegistrationHandle();
		FAssetEditorToolkitRegistrationHandle(const FAssetEditorToolkitRegistrationHandle&) = delete;
		auto operator=(const FAssetEditorToolkitRegistrationHandle&) -> FAssetEditorToolkitRegistrationHandle& = delete;
		DURINED_API FAssetEditorToolkitRegistrationHandle(FAssetEditorToolkitRegistrationHandle&& Other) noexcept;
		DURINED_API auto operator=(FAssetEditorToolkitRegistrationHandle&& Other) noexcept -> FAssetEditorToolkitRegistrationHandle&;

		auto IsValid() const -> bool { return RegistrationId != 0 && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
		DURINED_API auto Reset() -> void;

	private:
		friend class FEditorDocumentManager;
		DURINED_API FAssetEditorToolkitRegistrationHandle(
			std::weak_ptr<Detail::FEditorDocumentRegistryState> InState,
			uint64 InRegistrationId
		);

		std::weak_ptr<Detail::FEditorDocumentRegistryState> State;
		uint64 RegistrationId = 0;
	};

	// Owns toolkit routes, document identity, activation, persistence and close decisions.
	// Tab presentation and docking belong to FTabManager and TabLayout.
	// Every resolved asset document retains its asset until closed or unregistered.
	class FEditorDocumentManager final : private IAssetMoveObserver
	{
	public:
		DURINED_API explicit FEditorDocumentManager(DAssetEditorSubsystem* AssetEditors = nullptr);
		DURINED_API ~FEditorDocumentManager();
		FEditorDocumentManager(const FEditorDocumentManager&) = delete;
		auto operator=(const FEditorDocumentManager&) -> FEditorDocumentManager& = delete;

		// Keep provider code mapped until registration and all borrowed toolkit
		// shared pointers are released; removal does not invalidate external copies.
		DURINED_API auto RegisterBatch(
			FAssetEditorToolkitRegistrationBatch Batch) -> FAssetEditorToolkitRegistrationHandle;
		DURINED_API auto OpenDocument(FDocumentRequest Request) -> FDocumentId;
		DURINED_API auto CompleteDeferredDocumentOpen(FDocumentId DocumentId, bool bSucceeded) -> bool;
		DURINED_API auto OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool;
		DURINED_API auto CanOpenAsset(std::string_view AssetClassName) const -> bool;
		DURINED_API auto ActivateDocument(FDocumentId DocumentId) -> bool;
		DURINED_API auto ActivateToolkit(const FAssetEditorToolkitId& ToolkitId) -> bool;
		DURINED_API auto OpenDefaultToolkits() -> bool;
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
		DURINED_API auto FindToolkit(const FAssetEditorToolkitId& ToolkitId) const -> std::shared_ptr<IAssetEditorToolkit>;
		DURINED_API auto GetRegisteredToolkits() const -> std::vector<std::shared_ptr<IAssetEditorToolkit>>;
		DURINED_API auto GetToolkitDescriptors() const -> std::vector<FAssetEditorToolkitDescriptor>;

	private:
		auto SynchronizeAssetEditors() -> void;
		auto OnAssetsRelocated(
			std::span<const FAssetRelocationMapping> Mappings) -> void override;
		auto FindDocument(FDocumentId DocumentId) -> FEditorDocument*;
		auto FindDocument(const FAssetEditorToolkitId& ToolkitId, std::string_view DocumentKey) -> FEditorDocument*;
		auto RequestDeactivateActiveDocument(FDocumentId NextDocumentId = {}) -> bool;
		static auto AssetLabel(std::string_view ResourceId) -> std::string;

		std::shared_ptr<Detail::FEditorDocumentRegistryState> State;
		FAssetMoveObserverHandle AssetMoveObserverHandle = 0;
	};
}
