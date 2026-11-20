#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"
#include "Toolkits/AssetEditorTypes.h"
#include "AssetDefinition/AssetOpenTypes.h"
#include "DObject/WeakObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "Toolkits/ToolkitUI.h"
#include "Framework/TabManager.h"
#include "Toolkits/IAssetEditorInstance.h"

namespace Durin { class DPackage; class IObjectReplacementParticipant; }

namespace Durin
{
	struct FAssetEditorUnsavedChange
	{
		std::string Label;
		std::string ResourcePath;
	};

	class DAssetEditor;
	class FToolkitAssetReloadParticipant;
	struct FAssetEditorInitArgs;

	// Editor-instance commands enforce View policy before feature callbacks.
	class FAssetEditorToolkit : public IAssetEditorInstance, public std::enable_shared_from_this<FAssetEditorToolkit>
	{
	public:
		DURINED_API ~FAssetEditorToolkit() override;
		DURINED_API FAssetEditorToolkit();
		DURINED_API auto GetEditorInstance() const -> IAssetEditorInstance*;
		DURINED_API auto GetAssetEditorSubsystem() const -> DAssetEditorSubsystem*;
		DURINED_API auto GetToolkitHost() const -> std::shared_ptr<IToolkitHost>;
		DURINED_API auto GetToolkitMode() const -> EToolkitMode;
		auto GetOpenLocation() const -> EAssetEditorOpenLocation { return OpenLocation; }
		DURINED_API auto GetInstanceKey() const -> std::string_view;
		DURINED_API auto GetWindowId() const -> std::string_view;
		auto GetInstanceId() const -> std::string_view override { return GetWindowId(); }
		DURINED_API auto GetEditingObjects() const -> std::vector<DObject*>;
		// A concrete session may own its source directly. Otherwise this Toolkit owns the asset list.
		virtual auto GetOwnedEditingObjects() const -> std::optional<std::vector<DObject*>> { return std::nullopt; }
		DURINED_API auto SupportsRestore() const -> bool;
		DURINED_API auto GetEditorName() const -> FName override;
		DURINED_API auto IsPrimaryEditor() const -> bool override;
		DURINED_API auto GetOpenMethod() const -> EAssetOpenMethod override;
		DURINED_API auto GetLastActivationTime() -> double override;
		DURINED_API auto FocusWindow(DObject* Object = nullptr) -> void override;
		DURINED_API auto CloseWindow(EAssetEditorCloseReason Reason) -> bool override;
		auto IsClosed() const -> bool { return bClosed; }
		// CloseWindow asks permission before ToolkitManager releases ownership.
		DURINED_API virtual auto OnRequestClose(EAssetEditorCloseReason Reason) -> bool;

		DURINED_API virtual auto PrepareForExit() -> bool;
		DURINED_API virtual auto SaveForExit() -> bool;
		DURINED_API virtual auto GetUnsavedChanges() const -> std::vector<FAssetEditorUnsavedChange>;
		DURINED_API virtual auto GetEditorDisplayName() const -> std::string;
		virtual auto GetToolkitId() const -> const FAssetEditorToolkitId& = 0;
		virtual auto RequestDeactivate() -> bool { return true; }
		virtual auto OnPackageReloaded(DPackage*, DPackage*) -> void {}
		DURINED_API virtual auto CreateReloadParticipant() -> std::shared_ptr<IObjectReplacementParticipant>;
		virtual auto OnResourceIdRemapped(std::string_view, std::string_view) -> void {}
		virtual auto GetUndoDescription() const -> std::string { return {}; }
		virtual auto GetRedoDescription() const -> std::string { return {}; }
		virtual auto DrawFileMenu() -> void {}
		virtual auto DrawEditMenu() -> void {}
		virtual auto DrawWindowMenu() -> void {}
		virtual auto ResetLayout() -> void = 0;
		DURINED_API auto GetActiveAsset() const -> DObject*;
		DURINED_API auto ActivateAsset(DObject& Asset) -> bool;
		DURINED_API auto RequestCloseAsset(DObject& Asset) -> EAssetEditorCloseResult;
		DURINED_API auto IsAssetDirty(const DObject& Asset) const -> bool;
		DURINED_API auto SaveAsset(DObject& Asset) -> bool;
		DURINED_API auto DiscardAsset(DObject& Asset) -> bool;

		virtual auto GetLastOperationError() const -> std::string { return {}; }
		auto IsReadOnly() const -> bool { return OpenMethod == EAssetOpenMethod::View || !CanEditAssets(); }
		DURINED_API auto IsEditorOpen() const -> bool;
		DURINED_API auto DrawToolkit(bool bActive) -> bool;
		virtual auto GetLayoutVersion() const -> uint32 { return 1; }
		virtual auto GetLayoutName() const -> std::string_view { return GetToolkitId().GetValue(); }
		DURINED_API auto GetLayoutProfileKey() const -> std::string;
		DURINED_API auto GetPresentationRootKey(std::string_view EditorRootKey) const -> std::string;
		// Called once after closing is committed, while host/rendering services exist.
		virtual auto OnClose() -> void {}
		auto GetOwningAssetEditor() const -> DAssetEditor* { return OwningAssetEditor.Get(); }
		auto CanSaveActiveDocument() const -> bool { return IsEditorOpen() && !IsReadOnly() && OnCanSaveActiveDocument(); }
		auto SaveActiveDocument() -> bool { return ExecuteEditorCommand([&] { return OnSaveActiveDocument(); }); }
		auto CanUndo() const -> bool { return IsEditorOpen() && !IsReadOnly() && OnCanUndo(); }
		auto CanRedo() const -> bool { return IsEditorOpen() && !IsReadOnly() && OnCanRedo(); }
		auto Undo() -> bool { return ExecuteEditorCommand([&] { return OnUndo(); }); }
		auto Redo() -> bool { return ExecuteEditorCommand([&] { return OnRedo(); }); }
	protected:
		virtual auto CanEditAssets() const -> bool { return true; }
		DURINED_API auto NotifyEditingObjectsChanged() -> void;
		virtual auto FocusEditorView() -> void {}
		virtual auto PostInitAssetEditor() -> void {}
		virtual auto OnDrawToolkit(bool) -> bool { return false; }
		virtual auto OnOpenAsset(DObject&) -> bool { return true; }
		virtual auto OnActivateAsset(DObject&) -> void {}
		DURINED_API virtual auto OnRequestCloseAsset(DObject& Asset) -> EAssetEditorCloseResult;
		virtual auto OnCloseAsset(DObject&) -> void {}
		virtual auto OnSaveAsset(DObject&) -> bool { return false; }
		virtual auto OnDiscardAsset(DObject&) -> bool { return false; }
		virtual auto OnIsAssetDirty(const DObject&) const -> bool { return false; }
		virtual auto OnCanSaveActiveDocument() const -> bool { return false; }
		DURINED_API virtual auto OnSaveActiveDocument() -> bool;
		DURINED_API auto DrawAssetTabs(std::string_view RootKey,
			const std::function<void(DObject&)>& DrawAsset,
			const std::function<void(DObject&, bool)>& UpdateVisibility = {}) -> bool;
		virtual auto OnCanUndo() const -> bool { return false; }
		virtual auto OnCanRedo() const -> bool { return false; }
		virtual auto OnUndo() -> bool { return false; }
		virtual auto OnRedo() -> bool { return false; }
	private:
		friend class DAssetEditorSubsystem;
		friend class DAssetEditor;
		friend class FToolkitManager;
		auto NotifyClosed() -> void;
		auto CloseWindowInternal(EAssetEditorCloseReason Reason, bool bPublishRequest) -> bool;
		friend class FToolkitAssetReloadParticipant;
		auto InitAssetEditor(const FAssetEditorInitArgs& Args, FStrongObjectPtr Provider, DAssetEditor* Editor = nullptr) -> bool;
		auto HasEditingObject(const DObject& Object) const -> bool;
		auto TryCloseAsset(DObject& Asset) -> EAssetEditorCloseResult;
		auto ResolveAssetClose(FObjectKey& Key, EAssetEditorCloseResponse Response) -> EAssetEditorCloseResult;
		DURINED_API auto ExecuteEditorCommand(const std::function<bool()>& Command) -> bool;
		TStrongObjectPtr<DAssetEditor> OwningAssetEditor;
		std::vector<FStrongObjectPtr> EditingAssets;
		FObjectKey ActiveAsset;
		FTabManager Tabs;
		FStrongObjectPtr DefinitionProvider;
		EAssetOpenMethod OpenMethod = EAssetOpenMethod::Edit;
		EToolkitMode ToolkitMode = EToolkitMode::Standalone;
		EAssetEditorOpenLocation OpenLocation = EAssetEditorOpenLocation::MainWindow;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditors;
		std::weak_ptr<IToolkitHost> Host;
		// Identity comparison only, including during host destruction when pinning fails.
		const IToolkitHost* HostIdentity = nullptr;
		std::string InstanceKey;
		std::string WindowId;
		double LastActivationTime = 0;
		std::optional<EAssetEditorCloseReason> ActiveCloseReason;
		bool bPrimary = true;
		bool bInitialized = false;
		bool bClosed = false;
	};

	// Common panel defaults are opt-in; graph-only editors allocate no default viewport.
	class FBaseAssetToolkit : public FAssetEditorToolkit
	{
	public:
		DURINED_API FBaseAssetToolkit();
		DURINED_API ~FBaseAssetToolkit() override;
		DURINED_API auto RegisterUIExtension(FToolkitUIExtension Extension) -> FToolkitUIRegistration;
		DURINED_API auto InvokeCommand(std::string_view Id) -> bool;
		DURINED_API auto CanInvokeCommand(std::string_view Id) const -> bool;
		DURINED_API auto SetPanelOpen(std::string_view Id, bool bOpen) -> bool;
		DURINED_API auto IsPanelOpen(std::string_view Id) const -> bool;
		DURINED_API auto ResetPanels() -> void;
		DURINED_API auto DrawRegisteredUI() -> void;
		DURINED_API auto DrawWindowMenu() -> void override;
		DURINED_API auto OnClose() -> void override;
		// Inject a storage file for host/test isolation; otherwise uses project launch settings.
		DURINED_API auto RestorePanelLayout(std::string FilePath = {}) -> bool;
		// Existing ImGui configuration stores named templates, never per-window snapshot state.
		DURINED_API auto RestoreDockLayout(std::string_view Slot, const FAssetEditorToolkitId& DockType,
			uint32 Version, std::span<const std::string_view> Panels) -> void;
		DURINED_API auto SaveDockLayout(std::string_view Slot, const FAssetEditorToolkitId& DockType,
			uint32 Version, std::span<const std::string_view> Panels) -> void;
	protected:
		DURINED_API auto PostInitAssetEditor() -> void override;
		virtual auto CreateDefaultViewportPanel() -> std::optional<FToolkitPanelSpawner> { return {}; }
		virtual auto CreateDefaultDetailsPanel() -> std::optional<FToolkitPanelSpawner> { return {}; }
	private:
		std::shared_ptr<FToolkitUIState> UI;
		FToolkitUIRegistration DefaultPanels;
	};
}
