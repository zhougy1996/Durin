#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Documents/EditorDocumentTypes.h"
#include "AssetDefinition/AssetOpenTypes.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "Toolkits/ToolkitUI.h"

namespace Durin { class DPackage; class IObjectReplacementParticipant; }

namespace Durin
{
	// Defines document, history, menu, and layout services hosted by the editor shell.
	class IAssetEditorToolkit
	{
	public:
		virtual ~IAssetEditorToolkit() = default;

		DURINED_API virtual auto GetToolkitId() const -> const FAssetEditorToolkitId& = 0;
		// Deferred opens keep the current tab metadata until the toolkit reports completion.
		DURINED_API virtual auto OpenDocument(const FEditorDocument& Document) -> EDocumentOpenResult = 0;
		DURINED_API virtual auto ActivateDocument(const FEditorDocument& Document) -> void = 0;
		// Called before the manager changes the active document or toolkit.
		// Returning false keeps the current host state active.
		virtual auto RequestDeactivate() -> bool { return true; }
		// Release toolkit-owned document state only when returning Closed.
		virtual auto RequestCloseDocument(const FEditorDocument& Document) -> EDocumentCloseResult
		{
			return Document.bDirty ? EDocumentCloseResult::PendingConfirmation : EDocumentCloseResult::Closed;
		}
		// Resolve resource-specific persistence before the manager retries a pending close.
		virtual auto SaveDocument(const FEditorDocument& Document) -> bool
		{
			(void)Document;
			return false;
		}
		// Resolve resource-specific rollback before the manager retries a pending close.
		virtual auto DiscardDocument(const FEditorDocument& Document) -> bool
		{
			(void)Document;
			return false;
		}
		// Called during package reload publication, before the old graph retires.
		virtual auto OnPackageReloaded(DPackage*, DPackage*) -> void {}
		virtual auto CreateReloadParticipant() -> std::shared_ptr<IObjectReplacementParticipant> { return {}; }
		virtual auto OnResourceIdRemapped(std::string_view, std::string_view) -> void {}
		virtual auto IsDocumentDirty(const FEditorDocument& Document) const -> bool { return Document.bDirty; }
		virtual auto CanSaveActiveDocument() const -> bool { return false; }
		virtual auto SaveActiveDocument() -> bool { return false; }
		virtual auto CanUndo() const -> bool { return false; }
		virtual auto CanRedo() const -> bool { return false; }
		virtual auto GetUndoDescription() const -> std::string { return {}; }
		virtual auto GetRedoDescription() const -> std::string { return {}; }
		virtual auto Undo() -> bool { return false; }
		virtual auto Redo() -> bool { return false; }
		virtual auto DrawFileMenu() -> void {}
		virtual auto DrawEditMenu() -> void {}
		virtual auto DrawWindowMenu() -> void {}
		DURINED_API virtual auto DrawToolkit(bool bActive) -> bool = 0;
		DURINED_API virtual auto ResetLayout() -> void = 0;
	};

	class DAssetEditor;
	class FEditorDocumentManager;

	// Session-scoped common commands enforce View policy before feature callbacks.
	class FAssetEditorToolkit : public IAssetEditorToolkit, public std::enable_shared_from_this<FAssetEditorToolkit>
	{
	public:
		DURINED_API ~FAssetEditorToolkit() override;
		virtual auto GetLastOperationError() const -> std::string { return {}; }
		DURINED_API auto BindSession(DAssetEditor& Editor,
			std::shared_ptr<FEditorDocumentManager> Documents, EAssetOpenMethod Method,
			FStrongObjectPtr Provider) -> void;
		auto IsReadOnly() const -> bool { return OpenMethod == EAssetOpenMethod::View || !CanEditAssets(); }
		DURINED_API auto IsSessionActive() const -> bool;
		DURINED_API auto DrawToolkit(bool bActive) -> bool final;
		auto IsRetired() const -> bool { return bRetired; }
		auto Retire() -> void { if (!bRetired) { bRetired = true; ReleaseSessionUI(); } }
		virtual auto GetLayoutVersion() const -> uint32 { return 1; }
		virtual auto GetLayoutName() const -> std::string_view { return GetToolkitId().GetValue(); }
		DURINED_API auto GetLayoutProfileKey() const -> std::string;
		DURINED_API auto GetPresentationRootKey(std::string_view EditorRootKey) const -> std::string;
		DURINED_API auto GetPresentationDocument(const FEditorDocument& Document) const -> FEditorDocument;
		DURINED_API auto OpenRelatedAsset(std::string_view Path, std::string_view ClassName) -> bool;
		virtual auto ReleaseSessionUI() -> void {}
		virtual auto OnHostChanged() -> void {}
		auto GetOwningAssetEditor() const -> DAssetEditor* { return Owner.Get(); }
		auto SaveDocument(const FEditorDocument& Document) -> bool final
		{ return ExecuteSessionCommand([&] { return OnSaveDocument(Document); }); }
		auto DiscardDocument(const FEditorDocument& Document) -> bool final
		{ return ExecuteSessionCommand([&] { return OnDiscardDocument(Document); }); }
		auto IsDocumentDirty(const FEditorDocument& Document) const -> bool final
		{ return IsSessionActive() && !IsReadOnly() && OnIsDocumentDirty(Document); }
		auto CanSaveActiveDocument() const -> bool final { return IsSessionActive() && !IsReadOnly() && OnCanSaveActiveDocument(); }
		auto SaveActiveDocument() -> bool final { return ExecuteSessionCommand([&] { return OnSaveActiveDocument(); }); }
		auto CanUndo() const -> bool final { return IsSessionActive() && !IsReadOnly() && OnCanUndo(); }
		auto CanRedo() const -> bool final { return IsSessionActive() && !IsReadOnly() && OnCanRedo(); }
		auto Undo() -> bool final { return ExecuteSessionCommand([&] { return OnUndo(); }); }
		auto Redo() -> bool final { return ExecuteSessionCommand([&] { return OnRedo(); }); }
	protected:
		virtual auto CanEditAssets() const -> bool { return true; }
		virtual auto OnSessionBound() -> void {}
		virtual auto OnDrawToolkit(bool) -> bool { return false; }
		virtual auto OnSaveDocument(const FEditorDocument&) -> bool { return false; }
		virtual auto OnDiscardDocument(const FEditorDocument&) -> bool { return false; }
		virtual auto OnIsDocumentDirty(const FEditorDocument& Document) const -> bool { return Document.bDirty; }
		virtual auto OnCanSaveActiveDocument() const -> bool { return false; }
		virtual auto OnSaveActiveDocument() -> bool { return false; }
		virtual auto OnCanUndo() const -> bool { return false; }
		virtual auto OnCanRedo() const -> bool { return false; }
		virtual auto OnUndo() -> bool { return false; }
		virtual auto OnRedo() -> bool { return false; }
	private:
		DURINED_API auto ExecuteSessionCommand(const std::function<bool()>& Command) -> bool;
		TWeakObjectPtr<DAssetEditor> Owner;
		std::shared_ptr<FEditorDocumentManager> DocumentOwner;
		FStrongObjectPtr DefinitionProvider;
		EAssetOpenMethod OpenMethod = EAssetOpenMethod::Edit;
		bool bBoundToSession = false;
		bool bRetired = false;
	};

	// Common panel defaults are opt-in; graph-only editors allocate no default viewport.
	class FBaseAssetToolkit : public FAssetEditorToolkit
	{
	public:
		DURINED_API explicit FBaseAssetToolkit(bool bViewport = false, bool bDetails = false);
		DURINED_API ~FBaseAssetToolkit() override;
		DURINED_API auto RegisterUIExtension(FToolkitUIExtension Extension) -> FToolkitUIRegistration;
		DURINED_API auto InvokeCommand(std::string_view Id) -> bool;
		DURINED_API auto CanInvokeCommand(std::string_view Id) const -> bool;
		DURINED_API auto SetPanelOpen(std::string_view Id, bool bOpen) -> bool;
		DURINED_API auto IsPanelOpen(std::string_view Id) const -> bool;
		DURINED_API auto ResetPanels() -> void;
		DURINED_API auto DrawRegisteredUI() -> void;
		DURINED_API auto DrawWindowMenu() -> void override;
		DURINED_API auto ReleaseSessionUI() -> void override;
		DURINED_API auto OnHostChanged() -> void override;
		// Inject a storage file for host/test isolation; otherwise uses project launch settings.
		DURINED_API auto RestorePanelLayout(std::string FilePath = {}) -> bool;
		// Existing ImGui configuration stores named templates, never per-window snapshot state.
		DURINED_API auto RestoreDockLayout(std::string_view Slot, const FAssetEditorToolkitId& DockType,
			uint32 Version, std::span<const std::string_view> Panels) -> void;
		DURINED_API auto SaveDockLayout(std::string_view Slot, const FAssetEditorToolkitId& DockType,
			uint32 Version, std::span<const std::string_view> Panels) -> void;
		auto WantsDefaultViewport() const -> bool { return bCreateViewport; }
		auto WantsDefaultDetails() const -> bool { return bCreateDetails; }
	protected:
		DURINED_API auto OnSessionBound() -> void override;
		virtual auto CreateDefaultViewportPanel() -> std::optional<FToolkitPanelSpawner> { return {}; }
		virtual auto CreateDefaultDetailsPanel() -> std::optional<FToolkitPanelSpawner> { return {}; }
	private:
		bool bCreateViewport;
		bool bCreateDetails;
		std::shared_ptr<FToolkitUIState> UI;
		FToolkitUIRegistration DefaultPanels;
	};
}
