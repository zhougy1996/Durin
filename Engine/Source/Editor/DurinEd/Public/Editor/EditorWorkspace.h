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

	struct FEditorAssetEditorRegistration
	{
		std::string AssetClassName;
		FEditorWorkspaceTypeId WorkspaceType;
		EEditorDocumentPolicy DocumentPolicy = EEditorDocumentPolicy::PerResource;
		std::string SingletonDocumentKey;
		std::string SingletonLabel;
		bool bClosable = true;
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
		virtual auto DrawMainMenu() -> void = 0;
		virtual auto DrawWorkspace(bool bActive) -> bool = 0;
		virtual auto ResetLayout() -> void = 0;
	};

	class FEditorWorkspaceManager
	{
	public:
		DURINED_API auto RegisterWorkspace(std::shared_ptr<IEditorWorkspace> Workspace) -> bool;
		DURINED_API auto RegisterAssetEditor(FEditorAssetEditorRegistration Registration) -> bool;
		DURINED_API auto OpenDocument(FEditorDocumentRequest Request) -> FEditorDocumentId;
		DURINED_API auto OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool;
		DURINED_API auto ActivateDocument(FEditorDocumentId DocumentId) -> bool;
		DURINED_API auto ActivateWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) -> bool;
		DURINED_API auto RequestCloseDocument(FEditorDocumentId DocumentId) -> bool;
		DURINED_API auto RefreshDocumentState() -> void;

		auto GetDocuments() const -> const std::vector<FEditorDocumentTab>& { return Documents; }
		DURINED_API auto GetActiveDocument() -> FEditorDocumentTab*;
		DURINED_API auto GetActiveDocument() const -> const FEditorDocumentTab*;
		DURINED_API auto FindWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) const -> std::shared_ptr<IEditorWorkspace>;
		DURINED_API auto GetRegisteredWorkspaces() const -> std::vector<std::shared_ptr<IEditorWorkspace>>;

	private:
		auto FindDocument(FEditorDocumentId DocumentId) -> FEditorDocumentTab*;
		auto FindDocument(const FEditorWorkspaceTypeId& WorkspaceType, std::string_view DocumentKey) -> FEditorDocumentTab*;
		static auto AssetLabel(std::string_view ResourceId) -> std::string;

		uint64 NextDocumentId = 1;
		FEditorDocumentId ActiveDocumentId;
		std::vector<FEditorDocumentTab> Documents;
		std::unordered_map<std::string, std::shared_ptr<IEditorWorkspace>> Workspaces;
		std::unordered_map<std::string, FEditorAssetEditorRegistration> AssetEditors;
	};
}
