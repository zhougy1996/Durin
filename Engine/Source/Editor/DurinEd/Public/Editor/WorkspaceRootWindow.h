#pragma once

#include "DurinEdAPI.h"
#include "Editor/WorkspaceTypes.h"
#include "MonaImGui.h"

namespace Durin { class DObject; }

namespace Durin::Editor
{
	class FWorkspaceManager;
	// Describes an optional dock space nested inside a workspace root window.
	struct FWorkspaceInternalDockSpace
	{
		FWorkspaceTypeId WorkspaceType;
		uint32 LayoutVersion = 0;
	};

	// Configures one frame of an editor workspace root window.
	struct FWorkspaceRootWindowConfig
	{
		std::string_view DisplayName;
		std::string_view RootKey;
		bool bDirty = false;
		bool bZeroPadding = false;
		bool bDockInEditorHost = true;
		ImGuiWindowFlags AdditionalFlags = ImGuiWindowFlags_None;
		std::optional<FWorkspaceInternalDockSpace> InternalDockSpace;
	};

	// Reports visibility, activation, focus, and close intent for one frame.
	struct FWorkspaceRootWindowState
	{
		bool bVisible = false;
		bool bFocused = false;
		bool bActivated = false;
		bool bCloseRequested = false;
	};

	// Balances ImGui root-window lifetime and tracks dock-tab activation.
	class FWorkspaceRootWindow
	{
	public:
		auto RequestFocus() -> void { bFocusRequested = true; }
		auto ResetActivationState() -> void { bWasDockTabSelected = false; }
		DURINED_API auto Begin(const FWorkspaceRootWindowConfig& Config) -> FWorkspaceRootWindowState;
		DURINED_API auto End() -> void;

	private:
		bool bFocusRequested = false;
		bool bWasDockTabSelected = false;
		bool bWindowBegun = false;
	};

	// Hosts the repeated root-window lifecycle for one per-resource workspace.
	class FWorkspaceDocumentHost
	{
	public:
		DURINED_API auto RequestFocus(FDocumentId DocumentId) -> void;
		// Draws every document of one workspace and defers manager mutation until iteration completes.
		DURINED_API auto DrawDocuments(
			FWorkspaceManager& WorkspaceManager,
			const FWorkspaceTypeId& WorkspaceType,
			std::string_view WorkspaceRootKey,
			const std::function<bool(const FDocumentTab&)>& CanDrawDocument,
			const std::function<void(const FDocumentTab&)>& DrawDocument,
			const std::function<void(const FDocumentTab&)>& PrepareDocument = {}
		) -> bool;

	private:
		std::unordered_map<uint64, FWorkspaceRootWindow> DocumentWindows;
	};

	// Composes the class-neutral package, focus, active-document, and transaction
	// behavior shared by editable asset workspaces. Concrete editors retain
	// loading, property finalization, previews, builds, and type-specific errors.
	class FEditableAssetDocumentModel
	{
	public:
		DURINED_API auto Activate(const FDocumentTab& Document, ::Durin::DObject* Object) -> bool;
		DURINED_API auto Close(std::string_view ResourceId) -> void;
		auto GetActiveResourceId() const -> std::string_view { return ActiveResourceId; }
		auto GetDocumentHost() -> FWorkspaceDocumentHost& { return DocumentHost; }

		DURINED_API auto IsDirty(const ::Durin::DObject* Object) const -> bool;
		DURINED_API auto CanSave(const ::Durin::DObject* Object) const -> bool;
		DURINED_API auto Save(::Durin::DObject* Object,
			const std::function<bool()>& BeforeSave,
			const std::function<void(std::string)>& ReportError) -> bool;
		DURINED_API auto Discard(::Durin::DObject* Object,
			const std::function<void()>& BeforeDiscard = {}) -> bool;

		DURINED_API auto CanUndo() const -> bool;
		DURINED_API auto CanRedo() const -> bool;
		DURINED_API auto GetUndoDescription() const -> std::string_view;
		DURINED_API auto GetRedoDescription() const -> std::string_view;
		DURINED_API auto Undo() -> bool;
		DURINED_API auto Redo() -> bool;

	private:
		FWorkspaceDocumentHost DocumentHost;
		std::string ActiveResourceId;
	};

	// Composes the root-window, focus, active-document, and immutable command
	// behavior shared by read-only per-resource asset inspectors.
	class FReadOnlyAssetDocumentModel
	{
	public:
		DURINED_API auto Activate(const FDocumentTab& Document, const ::Durin::DObject* Object) -> bool;
		DURINED_API auto Close(std::string_view ResourceId) -> void;
		auto GetActiveResourceId() const -> std::string_view { return ActiveResourceId; }
		auto GetDocumentHost() -> FWorkspaceDocumentHost& { return DocumentHost; }

		auto SaveDocument(const FDocumentTab&) const -> bool { return false; }
		auto DiscardDocument(const FDocumentTab&) const -> bool { return false; }
		auto IsDocumentDirty(const FDocumentTab&) const -> bool { return false; }
		auto CanSaveActiveDocument() const -> bool { return false; }
		auto SaveActiveDocument() const -> bool { return false; }

	private:
		FWorkspaceDocumentHost DocumentHost;
		std::string ActiveResourceId;
	};
}
