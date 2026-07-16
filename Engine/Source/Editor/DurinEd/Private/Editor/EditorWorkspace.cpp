#include "Editor/EditorWorkspace.h"

namespace Durin
{
	auto FEditorWorkspaceManager::RegisterWorkspace(std::shared_ptr<IEditorWorkspace> Workspace) -> bool
	{
		if (!Workspace || !Workspace->GetWorkspaceType().IsValid()) return false;
		const std::string Key(Workspace->GetWorkspaceType().GetValue());
		return Workspaces.emplace(Key, std::move(Workspace)).second;
	}

	auto FEditorWorkspaceManager::RegisterAssetEditor(FEditorAssetEditorRegistration Registration) -> bool
	{
		if (Registration.AssetClassName.empty() || !Registration.WorkspaceType.IsValid() || !FindWorkspace(Registration.WorkspaceType)) return false;
		if (Registration.DocumentPolicy == EEditorDocumentPolicy::Singleton &&
			(Registration.SingletonDocumentKey.empty() || Registration.SingletonLabel.empty()))
			return false;
		return AssetEditors.emplace(Registration.AssetClassName, std::move(Registration)).second;
	}

	auto FEditorWorkspaceManager::OpenDocument(FEditorDocumentRequest Request) -> FEditorDocumentId
	{
		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Request.WorkspaceType);
		if (!Workspace || Request.DocumentKey.empty() || Request.Label.empty()) return {};

		if (FEditorDocumentTab* Existing = FindDocument(Request.WorkspaceType, Request.DocumentKey))
		{
			FEditorDocumentTab Candidate = *Existing;
			Candidate.ResourceId = std::move(Request.ResourceId);
			Candidate.Label = std::move(Request.Label);
			Candidate.bClosable = Request.bClosable;
			if (!Workspace->OpenDocument(Candidate)) return {};
			*Existing = std::move(Candidate);
			ActiveDocumentId = Existing->Id;
			Workspace->ActivateDocument(*Existing);
			return Existing->Id;
		}

		FEditorDocumentTab Document;
		Document.Id.Value = NextDocumentId++;
		Document.WorkspaceType = std::move(Request.WorkspaceType);
		Document.DocumentKey = std::move(Request.DocumentKey);
		Document.ResourceId = std::move(Request.ResourceId);
		Document.Label = std::move(Request.Label);
		Document.bClosable = Request.bClosable;
		if (!Workspace->OpenDocument(Document)) return {};

		Documents.push_back(std::move(Document));
		ActiveDocumentId = Documents.back().Id;
		Workspace->ActivateDocument(Documents.back());
		return Documents.back().Id;
	}

	auto FEditorWorkspaceManager::OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool
	{
		const auto Registration = AssetEditors.find(std::string(AssetClassName));
		if (Registration == AssetEditors.end()) return false;

		const FEditorAssetEditorRegistration& AssetEditor = Registration->second;
		FEditorDocumentRequest Request;
		Request.WorkspaceType = AssetEditor.WorkspaceType;
		Request.ResourceId = std::move(ResourceId);
		Request.bClosable = AssetEditor.bClosable;
		if (AssetEditor.DocumentPolicy == EEditorDocumentPolicy::Singleton)
		{
			Request.DocumentKey = AssetEditor.SingletonDocumentKey;
			Request.Label = AssetEditor.SingletonLabel;
		}
		else
		{
			Request.DocumentKey = Request.ResourceId;
			Request.Label = AssetLabel(Request.ResourceId);
		}
		return OpenDocument(std::move(Request)).IsValid();
	}

	auto FEditorWorkspaceManager::ActivateDocument(FEditorDocumentId DocumentId) -> bool
	{
		FEditorDocumentTab* Document = FindDocument(DocumentId);
		if (!Document) return false;
		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Document->WorkspaceType);
		if (!Workspace) return false;
		ActiveDocumentId = DocumentId;
		Workspace->ActivateDocument(*Document);
		return true;
	}

	auto FEditorWorkspaceManager::ActivateWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) -> bool
	{
		if (const FEditorDocumentTab* ActiveDocument = GetActiveDocument(); ActiveDocument && ActiveDocument->WorkspaceType == WorkspaceType)
			return true;
		const auto Found = std::ranges::find(Documents, WorkspaceType, &FEditorDocumentTab::WorkspaceType);
		return Found != Documents.end() && ActivateDocument(Found->Id);
	}

	auto FEditorWorkspaceManager::RequestCloseDocument(FEditorDocumentId DocumentId) -> bool
	{
		const auto Found = std::ranges::find(Documents, DocumentId, &FEditorDocumentTab::Id);
		if (Found == Documents.end() || !Found->bClosable) return false;
		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Found->WorkspaceType);
		if (!Workspace || !Workspace->RequestCloseDocument(*Found)) return false;

		const size_t ClosedIndex = static_cast<size_t>(std::distance(Documents.begin(), Found));
		const bool bWasActive = ActiveDocumentId == DocumentId;
		Documents.erase(Found);
		if (!bWasActive) return true;

		ActiveDocumentId = {};
		if (Documents.empty()) return true;
		const size_t NextIndex = std::min(ClosedIndex, Documents.size() - 1);
		return ActivateDocument(Documents[NextIndex].Id);
	}

	auto FEditorWorkspaceManager::RefreshDocumentState() -> void
	{
		for (FEditorDocumentTab& Document : Documents)
		{
			if (const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Document.WorkspaceType))
				Document.bDirty = Workspace->IsDocumentDirty(Document);
		}
	}

	auto FEditorWorkspaceManager::GetActiveDocument() -> FEditorDocumentTab*
	{
		return FindDocument(ActiveDocumentId);
	}

	auto FEditorWorkspaceManager::GetActiveDocument() const -> const FEditorDocumentTab*
	{
		const auto Found = std::ranges::find(Documents, ActiveDocumentId, &FEditorDocumentTab::Id);
		return Found == Documents.end() ? nullptr : &*Found;
	}

	auto FEditorWorkspaceManager::FindWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) const -> std::shared_ptr<IEditorWorkspace>
	{
		const auto Found = Workspaces.find(std::string(WorkspaceType.GetValue()));
		return Found == Workspaces.end() ? nullptr : Found->second;
	}

	auto FEditorWorkspaceManager::GetRegisteredWorkspaces() const -> std::vector<std::shared_ptr<IEditorWorkspace>>
	{
		std::vector<std::shared_ptr<IEditorWorkspace>> Result;
		Result.reserve(Workspaces.size());
		for (const auto& [Type, Workspace] : Workspaces)
		{
			(void)Type;
			Result.push_back(Workspace);
		}
		return Result;
	}

	auto FEditorWorkspaceManager::FindDocument(FEditorDocumentId DocumentId) -> FEditorDocumentTab*
	{
		const auto Found = std::ranges::find(Documents, DocumentId, &FEditorDocumentTab::Id);
		return Found == Documents.end() ? nullptr : &*Found;
	}

	auto FEditorWorkspaceManager::FindDocument(const FEditorWorkspaceTypeId& WorkspaceType, std::string_view DocumentKey) -> FEditorDocumentTab*
	{
		const auto Found = std::ranges::find_if(Documents, [&](const FEditorDocumentTab& Document) {
			return Document.WorkspaceType == WorkspaceType && Document.DocumentKey == DocumentKey;
		});
		return Found == Documents.end() ? nullptr : &*Found;
	}

	auto FEditorWorkspaceManager::AssetLabel(std::string_view ResourceId) -> std::string
	{
		const size_t Separator = ResourceId.find_last_of("/\\");
		const std::string_view Leaf = Separator == std::string_view::npos ? ResourceId : ResourceId.substr(Separator + 1);
		return Leaf.empty() ? std::string(ResourceId) : std::string(Leaf);
	}
}
