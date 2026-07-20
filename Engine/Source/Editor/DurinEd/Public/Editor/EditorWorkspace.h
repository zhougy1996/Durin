#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
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

	struct FEditorDocumentId
	{
		uint64 Value = 0;

		auto IsValid() const -> bool { return Value != 0; }
		auto operator==(const FEditorDocumentId&) const -> bool = default;
	};

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

	struct FEditorDocumentRequest
	{
		FEditorWorkspaceTypeId WorkspaceType;
		std::string DocumentKey;
		std::string ResourceId;
		std::string Label;
		bool bClosable = true;
	};

	enum class EEditorDocumentPolicy : uint8
	{
		Singleton,
		PerResource,
	};

	enum class EEditorWorkspaceHostDockPreference : uint8
	{
		None,
		Center,
	};

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

	struct FEditorWorkspaceRegistration
	{
		FEditorWorkspaceDescriptor Descriptor;
		std::shared_ptr<IEditorWorkspace> Workspace;
	};

	struct FEditorWorkspaceRegistrationBatch
	{
		std::vector<FEditorWorkspaceRegistration> Workspaces;
		std::vector<FEditorAssetEditorRegistration> AssetEditors;
	};

	namespace Detail
	{
		struct FEditorWorkspaceRegistryState;
	}

	class DURINED_API FEditorWorkspaceRegistrationHandle
	{
	public:
		FEditorWorkspaceRegistrationHandle() = default;
		~FEditorWorkspaceRegistrationHandle();
		FEditorWorkspaceRegistrationHandle(const FEditorWorkspaceRegistrationHandle&) = delete;
		auto operator=(const FEditorWorkspaceRegistrationHandle&) -> FEditorWorkspaceRegistrationHandle& = delete;
		FEditorWorkspaceRegistrationHandle(FEditorWorkspaceRegistrationHandle&& Other) noexcept;
		auto operator=(FEditorWorkspaceRegistrationHandle&& Other) noexcept -> FEditorWorkspaceRegistrationHandle&;

		auto IsValid() const -> bool { return RegistrationId != 0 && !State.expired(); }
		explicit operator bool() const { return IsValid(); }
		auto Reset() -> void;

	private:
		friend class FEditorWorkspaceManager;
		FEditorWorkspaceRegistrationHandle(std::weak_ptr<Detail::FEditorWorkspaceRegistryState> InState, uint64 InRegistrationId);

		std::weak_ptr<Detail::FEditorWorkspaceRegistryState> State;
		uint64 RegistrationId = 0;
	};

	class DURINED_API IEditorWorkspace
	{
	public:
		virtual ~IEditorWorkspace() = default;

		virtual auto GetWorkspaceType() const -> const FEditorWorkspaceTypeId& = 0;
		virtual auto OpenDocument(const FEditorDocumentTab& Document) -> bool = 0;
		virtual auto ActivateDocument(const FEditorDocumentTab& Document) -> void = 0;
		virtual auto RequestCloseDocument(const FEditorDocumentTab& Document) -> bool { return !Document.bDirty; }
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
		virtual auto DrawApplicationMenus() -> void {}
		virtual auto DrawWindowMenu() -> void {}
		virtual auto DrawWorkspace(bool bActive) -> bool = 0;
		virtual auto ResetLayout() -> void = 0;
	};

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
		DURINED_API auto OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool;
		DURINED_API auto ActivateDocument(FEditorDocumentId DocumentId) -> bool;
		DURINED_API auto ActivateWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) -> bool;
		DURINED_API auto OpenDefaultWorkspaces() -> bool;
		DURINED_API auto RequestCloseDocument(FEditorDocumentId DocumentId) -> bool;
		DURINED_API auto RefreshDocumentState() -> void;

		DURINED_API auto GetDocuments() const -> const std::vector<FEditorDocumentTab>&;
		DURINED_API auto GetActiveDocument() -> FEditorDocumentTab*;
		DURINED_API auto GetActiveDocument() const -> const FEditorDocumentTab*;
		DURINED_API auto FindWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) const -> std::shared_ptr<IEditorWorkspace>;
		DURINED_API auto GetRegisteredWorkspaces() const -> std::vector<std::shared_ptr<IEditorWorkspace>>;
		DURINED_API auto GetWorkspaceDescriptors() const -> std::vector<FEditorWorkspaceDescriptor>;

	private:
		auto FindDocument(FEditorDocumentId DocumentId) -> FEditorDocumentTab*;
		auto FindDocument(const FEditorWorkspaceTypeId& WorkspaceType, std::string_view DocumentKey) -> FEditorDocumentTab*;
		static auto AssetLabel(std::string_view ResourceId) -> std::string;

		std::shared_ptr<Detail::FEditorWorkspaceRegistryState> State;
		std::vector<FEditorWorkspaceRegistrationHandle> LegacyRegistrations;
	};
}
