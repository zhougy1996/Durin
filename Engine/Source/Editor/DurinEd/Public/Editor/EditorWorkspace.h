#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	// Provides a stable string identity for one editor workspace kind.
	class FEditorWorkspaceTypeId
	{
	public:
		FEditorWorkspaceTypeId() = default;
		explicit FEditorWorkspaceTypeId(std::string Value)
			: Value(std::move(Value))
		{
		}

		auto GetValue() const -> std::string_view { return Value; }
		auto IsValid() const -> bool { return !Value.empty(); }
		auto operator==(const FEditorWorkspaceTypeId&) const -> bool = default;

	private:
		std::string Value;
	};

	// Identifies an open editor document within the current workspace session.
	struct FEditorDocumentId
	{
		uint64 Value = 0;

		auto IsValid() const -> bool { return Value != 0; }
		auto operator==(const FEditorDocumentId&) const -> bool = default;
	};

	// Stores host-visible state for one open editor document tab.
	struct FEditorDocumentTab
	{
		FEditorDocumentId Id;
		FEditorWorkspaceTypeId WorkspaceType;
		std::string DocumentKey;
		std::string ResourceId;
		std::string Label;
		bool bDirty = false;
		bool bClosable = true;
	};

	// Describes a request to open or activate a workspace document.
	struct FEditorDocumentRequest
	{
		FEditorWorkspaceTypeId WorkspaceType;
		std::string DocumentKey;
		std::string ResourceId;
		std::string Label;
		bool bClosable = true;
	};

	// Reports whether a workspace rejected, completed, or deferred an open request.
	enum class EEditorDocumentOpenResult : uint8
	{
		Rejected,
		Opened,
		Deferred,
	};

	// Reports whether a close request completed, needs confirmation, was rejected, or was cancelled.
	enum class EEditorDocumentCloseResult : uint8
	{
		Rejected,
		Closed,
		PendingConfirmation,
		Cancelled,
	};

	// Selects how the host resolves a pending dirty-document close.
	enum class EEditorDocumentCloseResponse : uint8
	{
		Save,
		Discard,
		Cancel,
	};

	// Selects whether an asset editor shares one document or opens per resource.
	enum class EEditorDocumentPolicy : uint8
	{
		Singleton,
		PerResource,
	};

	// Suggests where a workspace should dock when first hosted.
	enum class EEditorWorkspaceHostDockPreference : uint8
	{
		None,
		Center,
	};

	// Defines host-facing metadata and singleton-document policy for a workspace.
	struct FEditorWorkspaceDescriptor
	{
		FEditorWorkspaceTypeId WorkspaceType;
		std::string DisplayName;
		std::string RootKey;
		bool bShowInWindowMenu = true;
		bool bOpenByDefault = false;
		EEditorWorkspaceHostDockPreference DefaultHostDockPreference = EEditorWorkspaceHostDockPreference::Center;
		std::string SingletonDocumentKey;
		std::string SingletonDocumentLabel;
		bool bSingletonDocumentClosable = true;

		auto HasSingletonDocument() const -> bool
		{
			return !SingletonDocumentKey.empty() && !SingletonDocumentLabel.empty();
		}
	};

	// Maps an asset class to the workspace and document policy that edits it.
	struct FEditorAssetEditorRegistration
	{
		std::string AssetClassName;
		FEditorWorkspaceTypeId WorkspaceType;
		EEditorDocumentPolicy DocumentPolicy = EEditorDocumentPolicy::PerResource;
		std::string SingletonDocumentKey;
		std::string SingletonLabel;
		bool bClosable = true;
	};

	class IEditorWorkspace;

	// Couples a workspace implementation with its host descriptor.
	struct FEditorWorkspaceRegistration
	{
		FEditorWorkspaceDescriptor Descriptor;
		std::shared_ptr<IEditorWorkspace> Workspace;
	};

	// Registers a dependency-consistent group of workspaces and asset editors.
	struct FEditorWorkspaceRegistrationBatch
	{
		std::vector<FEditorWorkspaceRegistration> Workspaces;
		std::vector<FEditorAssetEditorRegistration> AssetEditors;
	};

	namespace Detail
	{
		// Shared registry storage kept alive independently from registration handles.
		struct FEditorWorkspaceRegistryState;
	}

	// Owns one registry lease and unregisters its batch when reset or destroyed.
	class FEditorWorkspaceRegistrationHandle
	{
	public:
		FEditorWorkspaceRegistrationHandle() = default;
		DURINED_API ~FEditorWorkspaceRegistrationHandle();
		FEditorWorkspaceRegistrationHandle(const FEditorWorkspaceRegistrationHandle&) = delete;
		auto operator=(const FEditorWorkspaceRegistrationHandle&) -> FEditorWorkspaceRegistrationHandle& = delete;
		DURINED_API FEditorWorkspaceRegistrationHandle(FEditorWorkspaceRegistrationHandle&& Other) noexcept;
		DURINED_API auto operator=(FEditorWorkspaceRegistrationHandle&& Other) noexcept -> FEditorWorkspaceRegistrationHandle&;

		auto IsValid() const -> bool { return RegistrationId != 0 && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
		DURINED_API auto Reset() -> void;

	private:
		friend class FEditorWorkspaceManager;
		DURINED_API FEditorWorkspaceRegistrationHandle(
			std::weak_ptr<Detail::FEditorWorkspaceRegistryState> InState,
			uint64 InRegistrationId
		);

		std::weak_ptr<Detail::FEditorWorkspaceRegistryState> State;
		uint64 RegistrationId = 0;
	};

	// Defines document, history, menu, and layout services hosted by the editor shell.
	class IEditorWorkspace
	{
	public:
		virtual ~IEditorWorkspace() = default;

		DURINED_API virtual auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& = 0;
		// Deferred opens keep the current tab metadata until the workspace reports completion.
		DURINED_API virtual auto OpenDocument(const FEditorDocumentTab& Document) -> EEditorDocumentOpenResult = 0;
		DURINED_API virtual auto ActivateDocument(const FEditorDocumentTab& Document) -> void = 0;
		// Called before the manager changes the active document or workspace.
		// Returning false keeps the current host state active.
		virtual auto RequestDeactivate() -> bool { return true; }
		// Release workspace-owned document state only when returning Closed.
		virtual auto RequestCloseDocument(const FEditorDocumentTab& Document) -> EEditorDocumentCloseResult
		{
			return Document.bDirty ? EEditorDocumentCloseResult::PendingConfirmation : EEditorDocumentCloseResult::Closed;
		}
		// Resolve resource-specific persistence before the manager retries a pending close.
		virtual auto SaveDocument(const FEditorDocumentTab& Document) -> bool
		{
			(void)Document;
			return false;
		}
		// Resolve resource-specific rollback before the manager retries a pending close.
		virtual auto DiscardDocument(const FEditorDocumentTab& Document) -> bool
		{
			(void)Document;
			return false;
		}
		virtual auto IsDocumentDirty(const FEditorDocumentTab& Document) const -> bool { return Document.bDirty; }
		virtual auto CanSaveActiveDocument() const -> bool { return false; }
		virtual auto SaveActiveDocument() -> bool { return false; }
		virtual auto CanUndo() const -> bool { return false; }
		virtual auto CanRedo() const -> bool { return false; }
		virtual auto GetUndoDescription() const -> std::string_view { return {}; }
		virtual auto GetRedoDescription() const -> std::string_view { return {}; }
		virtual auto Undo() -> bool { return false; }
		virtual auto Redo() -> bool { return false; }
		virtual auto DrawFileMenu() -> void {}
		virtual auto DrawEditMenu() -> void {}
		virtual auto DrawWindowMenu() -> void {}
		DURINED_API virtual auto DrawWorkspace(bool bActive) -> bool = 0;
		DURINED_API virtual auto ResetLayout() -> void = 0;
	};

	// Owns workspace registrations, open documents, and active-host transitions.
	class FEditorWorkspaceManager
	{
	public:
		DURINED_API FEditorWorkspaceManager();
		DURINED_API ~FEditorWorkspaceManager();
		FEditorWorkspaceManager(const FEditorWorkspaceManager&) = delete;
		auto operator=(const FEditorWorkspaceManager&) -> FEditorWorkspaceManager& = delete;

		DURINED_API auto RegisterBatch(FEditorWorkspaceRegistrationBatch Batch) -> FEditorWorkspaceRegistrationHandle;
		DURINED_API auto RegisterWorkspace(FEditorWorkspaceRegistration Registration) -> bool;
		DURINED_API auto RegisterAssetEditor(FEditorAssetEditorRegistration Registration) -> bool;
		DURINED_API auto OpenDocument(FEditorDocumentRequest Request) -> FEditorDocumentId;
		DURINED_API auto CompleteDeferredDocumentOpen(FEditorDocumentId DocumentId, bool bSucceeded) -> bool;
		DURINED_API auto OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool;
		DURINED_API auto ActivateDocument(FEditorDocumentId DocumentId) -> bool;
		DURINED_API auto ActivateWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) -> bool;
		DURINED_API auto OpenDefaultWorkspaces() -> bool;
		DURINED_API auto RequestCloseDocument(FEditorDocumentId DocumentId) -> EEditorDocumentCloseResult;
		// Applies one response to the single pending close without losing it on save or discard failure.
		DURINED_API auto ResolvePendingDocumentClose(EEditorDocumentCloseResponse Response) -> EEditorDocumentCloseResult;
		DURINED_API auto RefreshDocumentState() -> void;

		DURINED_API auto GetDocuments() const -> const std::vector<FEditorDocumentTab>&;
		DURINED_API auto GetActiveDocument() -> FEditorDocumentTab*;
		DURINED_API auto GetActiveDocument() const -> const FEditorDocumentTab*;
		DURINED_API auto GetPendingCloseDocument() const -> const FEditorDocumentTab*;
		DURINED_API auto FindWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) const -> std::shared_ptr<IEditorWorkspace>;
		DURINED_API auto GetRegisteredWorkspaces() const -> std::vector<std::shared_ptr<IEditorWorkspace>>;
		DURINED_API auto GetWorkspaceDescriptors() const -> std::vector<FEditorWorkspaceDescriptor>;

	private:
		auto FindDocument(FEditorDocumentId DocumentId) -> FEditorDocumentTab*;
		auto FindDocument(const FEditorWorkspaceTypeId& WorkspaceType, std::string_view DocumentKey) -> FEditorDocumentTab*;
		auto RequestDeactivateActiveDocument(FEditorDocumentId NextDocumentId = {}) -> bool;
		static auto AssetLabel(std::string_view ResourceId) -> std::string;

		std::shared_ptr<Detail::FEditorWorkspaceRegistryState> State;
		std::vector<FEditorWorkspaceRegistrationHandle> LegacyRegistrations;
	};
}
