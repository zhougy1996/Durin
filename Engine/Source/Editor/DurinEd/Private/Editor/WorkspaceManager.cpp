#include "Editor/WorkspaceManager.h"

namespace Durin::Editor::Detail
{
	// Couples a registered workspace with the batch lease that owns it.
	struct FRegisteredWorkspace
	{
		FWorkspaceDescriptor Descriptor;
		std::shared_ptr<IWorkspace> Workspace;
		uint64 RegistrationId = 0;
		// Registration order keeps host menus, initial tabs, and drawing deterministic despite map storage.
		uint64 RegistrationOrder = 0;
	};

	// Couples an asset-editor mapping with the batch lease that owns it.
	struct FRegisteredAssetEditor
	{
		FAssetEditorRegistration Registration;
		uint64 RegistrationId = 0;
	};

	// Owns registration tables and removes every entry associated with a lease.
	struct FWorkspaceRegistryState
	{
		uint64 NextRegistrationId = 1;
		uint64 NextWorkspaceRegistrationOrder = 1;
		uint64 NextDocumentId = 1;
		FDocumentId ActiveDocumentId;
		FDocumentId PendingCloseDocumentId;
		std::vector<FDocumentTab> Documents;
		std::unordered_map<uint64, FDocumentTab> DeferredDocumentOpens;
		std::unordered_map<std::string, FRegisteredWorkspace> Workspaces;
		std::unordered_map<std::string, FRegisteredAssetEditor> AssetEditors;
	};

	auto UnregisterWorkspaceBatch(FWorkspaceRegistryState& State, uint64 RegistrationId) -> void
	{
		std::unordered_set<std::string> RemovedWorkspaceTypes;
		for (const auto& [WorkspaceType, Entry] : State.Workspaces)
		{
			if (Entry.RegistrationId == RegistrationId) RemovedWorkspaceTypes.insert(WorkspaceType);
		}

		const bool bActiveDocumentRemoved = std::ranges::any_of(State.Documents, [&](const FDocumentTab& Document) {
			return Document.Id == State.ActiveDocumentId && RemovedWorkspaceTypes.contains(std::string(Document.WorkspaceType.GetValue()));
		});
		if (bActiveDocumentRemoved)
		{
			const auto Active = std::ranges::find(State.Documents, State.ActiveDocumentId, &FDocumentTab::Id);
			if (Active != State.Documents.end())
			{
				const auto Workspace = State.Workspaces.find(std::string(Active->WorkspaceType.GetValue()));
				requiref(Workspace == State.Workspaces.end() || Workspace->second.Workspace->RequestDeactivate(),
					"An editor workspace cannot be unloaded while an active property preview cannot be cancelled");
			}
		}
		// A module cannot leave documents backed by its code in the manager after unloading.
		std::erase_if(State.Documents, [&](const FDocumentTab& Document) {
			return RemovedWorkspaceTypes.contains(std::string(Document.WorkspaceType.GetValue()));
		});
		if (const auto PendingClose = std::ranges::find(
				State.Documents, State.PendingCloseDocumentId, &FDocumentTab::Id
			); PendingClose == State.Documents.end())
			State.PendingCloseDocumentId = {};
		std::erase_if(State.DeferredDocumentOpens, [&](const auto& Pair) {
			return RemovedWorkspaceTypes.contains(std::string(Pair.second.WorkspaceType.GetValue()));
		});
		std::erase_if(State.AssetEditors, [&](const auto& Pair) {
			return Pair.second.RegistrationId == RegistrationId ||
				RemovedWorkspaceTypes.contains(std::string(Pair.second.Registration.WorkspaceType.GetValue()));
		});
		std::erase_if(State.Workspaces, [RegistrationId](const auto& Pair) {
			return Pair.second.RegistrationId == RegistrationId;
		});

		if (!bActiveDocumentRemoved) return;
		State.ActiveDocumentId = {};
		if (State.Documents.empty()) return;
		FDocumentTab& NextDocument = State.Documents.front();
		const auto Workspace = State.Workspaces.find(std::string(NextDocument.WorkspaceType.GetValue()));
		if (Workspace == State.Workspaces.end()) return;
		State.ActiveDocumentId = NextDocument.Id;
		Workspace->second.Workspace->ActivateDocument(NextDocument);
	}
}
namespace Durin::Editor
{
	FWorkspaceRegistrationHandle::FWorkspaceRegistrationHandle(
		std::weak_ptr<Detail::FWorkspaceRegistryState> InState, uint64 InRegistrationId
	)
		: State(std::move(InState)), RegistrationId(InRegistrationId)
	{
	}

	FWorkspaceRegistrationHandle::~FWorkspaceRegistrationHandle()
	{
		Reset();
	}

	FWorkspaceRegistrationHandle::FWorkspaceRegistrationHandle(FWorkspaceRegistrationHandle&& Other) noexcept
		: State(std::move(Other.State)), RegistrationId(std::exchange(Other.RegistrationId, 0))
	{
	}

	auto FWorkspaceRegistrationHandle::operator=(FWorkspaceRegistrationHandle&& Other) noexcept -> FWorkspaceRegistrationHandle&
	{
		if (this == &Other) return *this;
		Reset();
		State = std::move(Other.State);
		RegistrationId = std::exchange(Other.RegistrationId, 0);
		return *this;
	}

	auto FWorkspaceRegistrationHandle::Reset() -> void
	{
		if (RegistrationId == 0) return;
		if (const std::shared_ptr<Detail::FWorkspaceRegistryState> PinnedState = State.lock())
			Detail::UnregisterWorkspaceBatch(*PinnedState, RegistrationId);
		State.reset();
		RegistrationId = 0;
	}

	FWorkspaceManager::FWorkspaceManager()
		: State(std::make_shared<Detail::FWorkspaceRegistryState>())
	{
	}

	FWorkspaceManager::~FWorkspaceManager() = default;

	auto FWorkspaceManager::RegisterBatch(FWorkspaceRegistrationBatch Batch) -> FWorkspaceRegistrationHandle
	{
		if (Batch.Workspaces.empty() && Batch.AssetEditors.empty()) return {};
		std::unordered_set<std::string> BatchWorkspaceTypes;
		std::unordered_set<std::string> BatchRootKeys;
		for (const FWorkspaceRegistration& Registration : Batch.Workspaces)
		{
			const FWorkspaceDescriptor& Descriptor = Registration.Descriptor;
			if (!Registration.Workspace || !Descriptor.WorkspaceType.IsValid() || Descriptor.DisplayName.empty() || Descriptor.RootKey.empty()) return {};
			if (Registration.Workspace->GetWorkspaceType() != Descriptor.WorkspaceType) return {};
			if (Descriptor.bOpenByDefault && !Descriptor.HasSingletonDocument()) return {};
			if (Descriptor.SingletonDocumentKey.empty() != Descriptor.SingletonDocumentLabel.empty()) return {};
			const std::string WorkspaceType(Descriptor.WorkspaceType.GetValue());
			if (State->Workspaces.contains(WorkspaceType) || !BatchWorkspaceTypes.insert(WorkspaceType).second) return {};
			for (const auto& [ExistingType, Existing] : State->Workspaces)
			{
				(void)ExistingType;
				if (Existing.Descriptor.RootKey == Descriptor.RootKey) return {};
			}
			if (!BatchRootKeys.insert(Descriptor.RootKey).second) return {};
		}

		std::unordered_set<std::string> BatchAssetClasses;
		for (const FAssetEditorRegistration& Registration : Batch.AssetEditors)
		{
			if (Registration.AssetClassName.empty() || !Registration.WorkspaceType.IsValid()) return {};
			if (State->AssetEditors.contains(Registration.AssetClassName) || !BatchAssetClasses.insert(Registration.AssetClassName).second) return {};
			const std::string WorkspaceType(Registration.WorkspaceType.GetValue());
			if (!State->Workspaces.contains(WorkspaceType) && !BatchWorkspaceTypes.contains(WorkspaceType)) return {};
			if (Registration.DocumentPolicy == EDocumentPolicy::Singleton &&
				(Registration.SingletonDocumentKey.empty() || Registration.SingletonLabel.empty()))
				return {};
		}

		// Validation is deliberately complete before mutation, so a malformed later
		// entry cannot leave an earlier workspace or asset route installed.
		const uint64 RegistrationId = State->NextRegistrationId++;
		for (FWorkspaceRegistration& Registration : Batch.Workspaces)
		{
			const std::string WorkspaceType(Registration.Descriptor.WorkspaceType.GetValue());
			State->Workspaces.emplace(WorkspaceType, Detail::FRegisteredWorkspace{
				.Descriptor = std::move(Registration.Descriptor),
				.Workspace = std::move(Registration.Workspace),
				.RegistrationId = RegistrationId,
				.RegistrationOrder = State->NextWorkspaceRegistrationOrder++,
			});
		}
		for (FAssetEditorRegistration& Registration : Batch.AssetEditors)
		{
			const std::string AssetClassName = Registration.AssetClassName;
			State->AssetEditors.emplace(AssetClassName, Detail::FRegisteredAssetEditor{
				.Registration = std::move(Registration),
				.RegistrationId = RegistrationId,
			});
		}
		return FWorkspaceRegistrationHandle(State, RegistrationId);
	}

	auto FWorkspaceManager::OpenDocument(FDocumentRequest Request) -> FDocumentId
	{
		const std::shared_ptr<IWorkspace> Workspace = FindWorkspace(Request.WorkspaceType);
		if (!Workspace || Request.DocumentKey.empty() || Request.Label.empty()) return {};

		if (FDocumentTab* Existing = FindDocument(Request.WorkspaceType, Request.DocumentKey))
		{
			const bool bReplacingActiveResource = State->ActiveDocumentId == Existing->Id
				&& Existing->ResourceId != Request.ResourceId;
			if (!RequestDeactivateActiveDocument(bReplacingActiveResource ? FDocumentId{} : Existing->Id)) return {};
			FDocumentTab Candidate = *Existing;
			Candidate.ResourceId = std::move(Request.ResourceId);
			Candidate.Label = std::move(Request.Label);
			Candidate.bClosable = Request.bClosable;
			const EDocumentOpenResult OpenResult = Workspace->OpenDocument(Candidate);
			if (OpenResult == EDocumentOpenResult::Rejected) return {};
			if (OpenResult == EDocumentOpenResult::Deferred)
			{
				State->DeferredDocumentOpens[Existing->Id.Value] = std::move(Candidate);
				return Existing->Id;
			}
			*Existing = std::move(Candidate);
			Workspace->ActivateDocument(*Existing);
			State->ActiveDocumentId = Existing->Id;
			return Existing->Id;
		}

		FDocumentTab Document;
		Document.Id.Value = State->NextDocumentId++;
		Document.WorkspaceType = std::move(Request.WorkspaceType);
		Document.DocumentKey = std::move(Request.DocumentKey);
		Document.ResourceId = std::move(Request.ResourceId);
		Document.Label = std::move(Request.Label);
		Document.bClosable = Request.bClosable;
		if (!RequestDeactivateActiveDocument()) return {};
		const EDocumentOpenResult OpenResult = Workspace->OpenDocument(Document);
		if (OpenResult == EDocumentOpenResult::Rejected) return {};
		if (OpenResult == EDocumentOpenResult::Deferred)
		{
			const FDocumentId DeferredId = Document.Id;
			State->DeferredDocumentOpens[DeferredId.Value] = std::move(Document);
			return DeferredId;
		}

		State->Documents.push_back(std::move(Document));
		Workspace->ActivateDocument(State->Documents.back());
		State->ActiveDocumentId = State->Documents.back().Id;
		return State->Documents.back().Id;
	}

	auto FWorkspaceManager::CompleteDeferredDocumentOpen(FDocumentId DocumentId, bool bSucceeded) -> bool
	{
		const auto Pending = State->DeferredDocumentOpens.find(DocumentId.Value);
		if (Pending == State->DeferredDocumentOpens.end()) return false;
		if (!bSucceeded)
		{
			State->DeferredDocumentOpens.erase(Pending);
			return true;
		}

		FDocumentTab Document = std::move(Pending->second);
		State->DeferredDocumentOpens.erase(Pending);
		const std::shared_ptr<IWorkspace> Workspace = FindWorkspace(Document.WorkspaceType);
		if (!Workspace) return false;

		if (FDocumentTab* Existing = FindDocument(DocumentId))
			*Existing = std::move(Document);
		else
			State->Documents.push_back(std::move(Document));

		FDocumentTab* OpenedDocument = FindDocument(DocumentId);
		if (!OpenedDocument) return false;
		Workspace->ActivateDocument(*OpenedDocument);
		State->ActiveDocumentId = OpenedDocument->Id;
		return true;
	}

	auto FWorkspaceManager::OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool
	{
		const auto Found = State->AssetEditors.find(std::string(AssetClassName));
		if (Found == State->AssetEditors.end()) return false;

		const FAssetEditorRegistration& AssetEditor = Found->second.Registration;
		FDocumentRequest Request;
		Request.WorkspaceType = AssetEditor.WorkspaceType;
		Request.ResourceId = std::move(ResourceId);
		Request.bClosable = AssetEditor.bClosable;
		if (AssetEditor.DocumentPolicy == EDocumentPolicy::Singleton)
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

	auto FWorkspaceManager::ActivateDocument(FDocumentId DocumentId) -> bool
	{
		FDocumentTab* Document = FindDocument(DocumentId);
		if (!Document) return false;
		const std::shared_ptr<IWorkspace> Workspace = FindWorkspace(Document->WorkspaceType);
		if (!Workspace) return false;
		if (!RequestDeactivateActiveDocument(DocumentId)) return false;
		Workspace->ActivateDocument(*Document);
		State->ActiveDocumentId = DocumentId;
		return true;
	}

	auto FWorkspaceManager::ActivateWorkspace(const FWorkspaceTypeId& WorkspaceType) -> bool
	{
		if (const FDocumentTab* ActiveDocument = GetActiveDocument(); ActiveDocument && ActiveDocument->WorkspaceType == WorkspaceType)
			return true;
		const auto Found = std::ranges::find(State->Documents, WorkspaceType, &FDocumentTab::WorkspaceType);
		return Found != State->Documents.end() && ActivateDocument(Found->Id);
	}

	auto FWorkspaceManager::OpenDefaultWorkspaces() -> bool
	{
		for (const FWorkspaceDescriptor& Descriptor : GetWorkspaceDescriptors())
		{
			if (!Descriptor.bOpenByDefault) continue;
			if (!Descriptor.HasSingletonDocument()) return false;
			if (!OpenDocument({
				.WorkspaceType = Descriptor.WorkspaceType,
				.DocumentKey = Descriptor.SingletonDocumentKey,
				.Label = Descriptor.SingletonDocumentLabel,
				.bClosable = Descriptor.bSingletonDocumentClosable,
			}).IsValid()) return false;
		}
		return true;
	}

	auto FWorkspaceManager::RequestCloseDocument(FDocumentId DocumentId) -> EDocumentCloseResult
	{
		if (State->PendingCloseDocumentId.IsValid())
		{
			return State->PendingCloseDocumentId == DocumentId
				? EDocumentCloseResult::PendingConfirmation
				: EDocumentCloseResult::Rejected;
		}
		if (State->DeferredDocumentOpens.erase(DocumentId.Value) != 0)
			return EDocumentCloseResult::Closed;
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FDocumentTab::Id);
		if (Found == State->Documents.end() || !Found->bClosable) return EDocumentCloseResult::Rejected;
		const std::shared_ptr<IWorkspace> Workspace = FindWorkspace(Found->WorkspaceType);
		if (!Workspace) return EDocumentCloseResult::Rejected;
		const EDocumentCloseResult CloseResult = Workspace->RequestCloseDocument(*Found);
		if (CloseResult == EDocumentCloseResult::PendingConfirmation)
		{
			State->PendingCloseDocumentId = DocumentId;
			return CloseResult;
		}
		if (CloseResult == EDocumentCloseResult::Rejected) return CloseResult;

		const size_t ClosedIndex = static_cast<size_t>(std::distance(State->Documents.begin(), Found));
		const bool bWasActive = State->ActiveDocumentId == DocumentId;
		State->Documents.erase(Found);
		if (!bWasActive) return EDocumentCloseResult::Closed;

		State->ActiveDocumentId = {};
		if (State->Documents.empty()) return EDocumentCloseResult::Closed;
		const size_t NextIndex = std::min(ClosedIndex, State->Documents.size() - 1);
		ActivateDocument(State->Documents[NextIndex].Id);
		return EDocumentCloseResult::Closed;
	}

	auto FWorkspaceManager::ResolvePendingDocumentClose(
		EDocumentCloseResponse Response
	) -> EDocumentCloseResult
	{
		FDocumentTab* Document = FindDocument(State->PendingCloseDocumentId);
		if (!Document)
		{
			State->PendingCloseDocumentId = {};
			return EDocumentCloseResult::Rejected;
		}
		if (Response == EDocumentCloseResponse::Cancel)
		{
			State->PendingCloseDocumentId = {};
			return EDocumentCloseResult::Cancelled;
		}

		const std::shared_ptr<IWorkspace> Workspace = FindWorkspace(Document->WorkspaceType);
		if (!Workspace) return EDocumentCloseResult::Rejected;
		const bool bResolved = Response == EDocumentCloseResponse::Save
			? Workspace->SaveDocument(*Document)
			: Workspace->DiscardDocument(*Document);
		if (!bResolved) return EDocumentCloseResult::PendingConfirmation;

		const FDocumentId DocumentId = Document->Id;
		State->PendingCloseDocumentId = {};
		return RequestCloseDocument(DocumentId);
	}

	auto FWorkspaceManager::RefreshDocumentState() -> void
	{
		for (FDocumentTab& Document : State->Documents)
		{
			if (const std::shared_ptr<IWorkspace> Workspace = FindWorkspace(Document.WorkspaceType))
				Document.bDirty = Workspace->IsDocumentDirty(Document);
		}
	}

	auto FWorkspaceManager::GetDocuments() const -> const std::vector<FDocumentTab>&
	{
		return State->Documents;
	}

	auto FWorkspaceManager::GetActiveDocument() -> FDocumentTab*
	{
		return FindDocument(State->ActiveDocumentId);
	}

	auto FWorkspaceManager::GetActiveDocument() const -> const FDocumentTab*
	{
		const auto Found = std::ranges::find(State->Documents, State->ActiveDocumentId, &FDocumentTab::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FWorkspaceManager::GetPendingCloseDocument() const -> const FDocumentTab*
	{
		const auto Found = std::ranges::find(State->Documents, State->PendingCloseDocumentId, &FDocumentTab::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FWorkspaceManager::FindWorkspace(const FWorkspaceTypeId& WorkspaceType) const -> std::shared_ptr<IWorkspace>
	{
		const auto Found = State->Workspaces.find(std::string(WorkspaceType.GetValue()));
		return Found == State->Workspaces.end() ? nullptr : Found->second.Workspace;
	}

	auto FWorkspaceManager::GetRegisteredWorkspaces() const -> std::vector<std::shared_ptr<IWorkspace>>
	{
		std::vector<const Detail::FRegisteredWorkspace*> Entries;
		Entries.reserve(State->Workspaces.size());
		for (const auto& [Type, Entry] : State->Workspaces)
		{
			(void)Type;
			Entries.push_back(&Entry);
		}
		std::ranges::sort(Entries, {}, &Detail::FRegisteredWorkspace::RegistrationOrder);
		std::vector<std::shared_ptr<IWorkspace>> Result;
		Result.reserve(Entries.size());
		for (const Detail::FRegisteredWorkspace* Entry : Entries) Result.push_back(Entry->Workspace);
		return Result;
	}

	auto FWorkspaceManager::GetWorkspaceDescriptors() const -> std::vector<FWorkspaceDescriptor>
	{
		std::vector<const Detail::FRegisteredWorkspace*> Entries;
		Entries.reserve(State->Workspaces.size());
		for (const auto& [Type, Entry] : State->Workspaces)
		{
			(void)Type;
			Entries.push_back(&Entry);
		}
		std::ranges::sort(Entries, {}, &Detail::FRegisteredWorkspace::RegistrationOrder);
		std::vector<FWorkspaceDescriptor> Result;
		Result.reserve(Entries.size());
		for (const Detail::FRegisteredWorkspace* Entry : Entries) Result.push_back(Entry->Descriptor);
		return Result;
	}

	auto FWorkspaceManager::FindDocument(FDocumentId DocumentId) -> FDocumentTab*
	{
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FDocumentTab::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FWorkspaceManager::FindDocument(const FWorkspaceTypeId& WorkspaceType, std::string_view DocumentKey) -> FDocumentTab*
	{
		const auto Found = std::ranges::find_if(State->Documents, [&](const FDocumentTab& Document) {
			return Document.WorkspaceType == WorkspaceType && Document.DocumentKey == DocumentKey;
		});
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FWorkspaceManager::RequestDeactivateActiveDocument(FDocumentId NextDocumentId) -> bool
	{
		if (!State->ActiveDocumentId.IsValid() || State->ActiveDocumentId == NextDocumentId) return true;
		const FDocumentTab* ActiveDocument = GetActiveDocument();
		if (!ActiveDocument) return true;
		const std::shared_ptr<IWorkspace> Workspace = FindWorkspace(ActiveDocument->WorkspaceType);
		return !Workspace || Workspace->RequestDeactivate();
	}

	auto FWorkspaceManager::AssetLabel(std::string_view ResourceId) -> std::string
	{
		const size_t Separator = ResourceId.find_last_of("/\\");
		const std::string_view Leaf = Separator == std::string_view::npos ? ResourceId : ResourceId.substr(Separator + 1);
		return Leaf.empty() ? std::string(ResourceId) : std::string(Leaf);
	}
}
