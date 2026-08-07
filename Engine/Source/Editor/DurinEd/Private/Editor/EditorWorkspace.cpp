#include "Editor/EditorWorkspace.h"

namespace Durin::Detail
{
	// Couples a registered workspace with the batch lease that owns it.
	struct FRegisteredWorkspace
	{
		FEditorWorkspaceDescriptor Descriptor;
		std::shared_ptr<IEditorWorkspace> Workspace;
		uint64 RegistrationId = 0;
		// Registration order keeps host menus, initial tabs, and drawing deterministic despite map storage.
		uint64 RegistrationOrder = 0;
	};

	// Couples an asset-editor mapping with the batch lease that owns it.
	struct FRegisteredAssetEditor
	{
		FEditorAssetEditorRegistration Registration;
		uint64 RegistrationId = 0;
	};

	// Owns registration tables and removes every entry associated with a lease.
	struct FEditorWorkspaceRegistryState
	{
		uint64 NextRegistrationId = 1;
		uint64 NextWorkspaceRegistrationOrder = 1;
		uint64 NextDocumentId = 1;
		FEditorDocumentId ActiveDocumentId;
		FEditorDocumentId PendingCloseDocumentId;
		std::vector<FEditorDocumentTab> Documents;
		std::unordered_map<uint64, FEditorDocumentTab> DeferredDocumentOpens;
		std::unordered_map<std::string, FRegisteredWorkspace> Workspaces;
		std::unordered_map<std::string, FRegisteredAssetEditor> AssetEditors;
	};

	auto UnregisterWorkspaceBatch(FEditorWorkspaceRegistryState& State, uint64 RegistrationId) -> void
	{
		std::unordered_set<std::string> RemovedWorkspaceTypes;
		for (const auto& [WorkspaceType, Entry] : State.Workspaces)
		{
			if (Entry.RegistrationId == RegistrationId) RemovedWorkspaceTypes.insert(WorkspaceType);
		}

		const bool bActiveDocumentRemoved = std::ranges::any_of(State.Documents, [&](const FEditorDocumentTab& Document) {
			return Document.Id == State.ActiveDocumentId && RemovedWorkspaceTypes.contains(std::string(Document.WorkspaceType.GetValue()));
		});
		if (bActiveDocumentRemoved)
		{
			const auto Active = std::ranges::find(State.Documents, State.ActiveDocumentId, &FEditorDocumentTab::Id);
			if (Active != State.Documents.end())
			{
				const auto Workspace = State.Workspaces.find(std::string(Active->WorkspaceType.GetValue()));
				requiref(Workspace == State.Workspaces.end() || Workspace->second.Workspace->RequestDeactivate(),
					"An editor workspace cannot be unloaded while an active property preview cannot be cancelled");
			}
		}
		// A module cannot leave documents backed by its code in the manager after unloading.
		std::erase_if(State.Documents, [&](const FEditorDocumentTab& Document) {
			return RemovedWorkspaceTypes.contains(std::string(Document.WorkspaceType.GetValue()));
		});
		if (const auto PendingClose = std::ranges::find(
				State.Documents, State.PendingCloseDocumentId, &FEditorDocumentTab::Id
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
		FEditorDocumentTab& NextDocument = State.Documents.front();
		const auto Workspace = State.Workspaces.find(std::string(NextDocument.WorkspaceType.GetValue()));
		if (Workspace == State.Workspaces.end()) return;
		State.ActiveDocumentId = NextDocument.Id;
		Workspace->second.Workspace->ActivateDocument(NextDocument);
	}
}

namespace Durin
{
	FEditorWorkspaceRegistrationHandle::FEditorWorkspaceRegistrationHandle(
		std::weak_ptr<Detail::FEditorWorkspaceRegistryState> InState, uint64 InRegistrationId
	)
		: State(std::move(InState)), RegistrationId(InRegistrationId)
	{
	}

	FEditorWorkspaceRegistrationHandle::~FEditorWorkspaceRegistrationHandle()
	{
		Reset();
	}

	FEditorWorkspaceRegistrationHandle::FEditorWorkspaceRegistrationHandle(FEditorWorkspaceRegistrationHandle&& Other) noexcept
		: State(std::move(Other.State)), RegistrationId(std::exchange(Other.RegistrationId, 0))
	{
	}

	auto FEditorWorkspaceRegistrationHandle::operator=(FEditorWorkspaceRegistrationHandle&& Other) noexcept -> FEditorWorkspaceRegistrationHandle&
	{
		if (this == &Other) return *this;
		Reset();
		State = std::move(Other.State);
		RegistrationId = std::exchange(Other.RegistrationId, 0);
		return *this;
	}

	auto FEditorWorkspaceRegistrationHandle::Reset() -> void
	{
		if (RegistrationId == 0) return;
		if (const std::shared_ptr<Detail::FEditorWorkspaceRegistryState> PinnedState = State.lock())
			Detail::UnregisterWorkspaceBatch(*PinnedState, RegistrationId);
		State.reset();
		RegistrationId = 0;
	}

	FEditorWorkspaceManager::FEditorWorkspaceManager()
		: State(std::make_shared<Detail::FEditorWorkspaceRegistryState>())
	{
	}

	FEditorWorkspaceManager::~FEditorWorkspaceManager() = default;

	auto FEditorWorkspaceManager::RegisterBatch(FEditorWorkspaceRegistrationBatch Batch) -> FEditorWorkspaceRegistrationHandle
	{
		if (Batch.Workspaces.empty() && Batch.AssetEditors.empty()) return {};
		std::unordered_set<std::string> BatchWorkspaceTypes;
		std::unordered_set<std::string> BatchRootKeys;
		for (const FEditorWorkspaceRegistration& Registration : Batch.Workspaces)
		{
			const FEditorWorkspaceDescriptor& Descriptor = Registration.Descriptor;
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
		for (const FEditorAssetEditorRegistration& Registration : Batch.AssetEditors)
		{
			if (Registration.AssetClassName.empty() || !Registration.WorkspaceType.IsValid()) return {};
			if (State->AssetEditors.contains(Registration.AssetClassName) || !BatchAssetClasses.insert(Registration.AssetClassName).second) return {};
			const std::string WorkspaceType(Registration.WorkspaceType.GetValue());
			if (!State->Workspaces.contains(WorkspaceType) && !BatchWorkspaceTypes.contains(WorkspaceType)) return {};
			if (Registration.DocumentPolicy == EEditorDocumentPolicy::Singleton &&
				(Registration.SingletonDocumentKey.empty() || Registration.SingletonLabel.empty()))
				return {};
		}

		// Validation is deliberately complete before mutation, so a malformed later
		// entry cannot leave an earlier workspace or asset route installed.
		const uint64 RegistrationId = State->NextRegistrationId++;
		for (FEditorWorkspaceRegistration& Registration : Batch.Workspaces)
		{
			const std::string WorkspaceType(Registration.Descriptor.WorkspaceType.GetValue());
			State->Workspaces.emplace(WorkspaceType, Detail::FRegisteredWorkspace{
				.Descriptor = std::move(Registration.Descriptor),
				.Workspace = std::move(Registration.Workspace),
				.RegistrationId = RegistrationId,
				.RegistrationOrder = State->NextWorkspaceRegistrationOrder++,
			});
		}
		for (FEditorAssetEditorRegistration& Registration : Batch.AssetEditors)
		{
			const std::string AssetClassName = Registration.AssetClassName;
			State->AssetEditors.emplace(AssetClassName, Detail::FRegisteredAssetEditor{
				.Registration = std::move(Registration),
				.RegistrationId = RegistrationId,
			});
		}
		return FEditorWorkspaceRegistrationHandle(State, RegistrationId);
	}

	auto FEditorWorkspaceManager::RegisterWorkspace(FEditorWorkspaceRegistration Registration) -> bool
	{
		FEditorWorkspaceRegistrationHandle Handle = RegisterBatch({.Workspaces = {std::move(Registration)}});
		if (!Handle) return false;
		LegacyRegistrations.push_back(std::move(Handle));
		return true;
	}

	auto FEditorWorkspaceManager::RegisterAssetEditor(FEditorAssetEditorRegistration Registration) -> bool
	{
		FEditorWorkspaceRegistrationHandle Handle = RegisterBatch({.AssetEditors = {std::move(Registration)}});
		if (!Handle) return false;
		LegacyRegistrations.push_back(std::move(Handle));
		return true;
	}

	auto FEditorWorkspaceManager::OpenDocument(FEditorDocumentRequest Request) -> FEditorDocumentId
	{
		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Request.WorkspaceType);
		if (!Workspace || Request.DocumentKey.empty() || Request.Label.empty()) return {};

		if (FEditorDocumentTab* Existing = FindDocument(Request.WorkspaceType, Request.DocumentKey))
		{
			const bool bReplacingActiveResource = State->ActiveDocumentId == Existing->Id
				&& Existing->ResourceId != Request.ResourceId;
			if (!RequestDeactivateActiveDocument(bReplacingActiveResource ? FEditorDocumentId{} : Existing->Id)) return {};
			FEditorDocumentTab Candidate = *Existing;
			Candidate.ResourceId = std::move(Request.ResourceId);
			Candidate.Label = std::move(Request.Label);
			Candidate.bClosable = Request.bClosable;
			const EEditorDocumentOpenResult OpenResult = Workspace->OpenDocument(Candidate);
			if (OpenResult == EEditorDocumentOpenResult::Rejected) return {};
			if (OpenResult == EEditorDocumentOpenResult::Deferred)
			{
				State->DeferredDocumentOpens[Existing->Id.Value] = std::move(Candidate);
				return Existing->Id;
			}
			*Existing = std::move(Candidate);
			Workspace->ActivateDocument(*Existing);
			State->ActiveDocumentId = Existing->Id;
			return Existing->Id;
		}

		FEditorDocumentTab Document;
		Document.Id.Value = State->NextDocumentId++;
		Document.WorkspaceType = std::move(Request.WorkspaceType);
		Document.DocumentKey = std::move(Request.DocumentKey);
		Document.ResourceId = std::move(Request.ResourceId);
		Document.Label = std::move(Request.Label);
		Document.bClosable = Request.bClosable;
		if (!RequestDeactivateActiveDocument()) return {};
		const EEditorDocumentOpenResult OpenResult = Workspace->OpenDocument(Document);
		if (OpenResult == EEditorDocumentOpenResult::Rejected) return {};
		if (OpenResult == EEditorDocumentOpenResult::Deferred)
		{
			const FEditorDocumentId DeferredId = Document.Id;
			State->DeferredDocumentOpens[DeferredId.Value] = std::move(Document);
			return DeferredId;
		}

		State->Documents.push_back(std::move(Document));
		Workspace->ActivateDocument(State->Documents.back());
		State->ActiveDocumentId = State->Documents.back().Id;
		return State->Documents.back().Id;
	}

	auto FEditorWorkspaceManager::CompleteDeferredDocumentOpen(FEditorDocumentId DocumentId, bool bSucceeded) -> bool
	{
		const auto Pending = State->DeferredDocumentOpens.find(DocumentId.Value);
		if (Pending == State->DeferredDocumentOpens.end()) return false;
		if (!bSucceeded)
		{
			State->DeferredDocumentOpens.erase(Pending);
			return true;
		}

		FEditorDocumentTab Document = std::move(Pending->second);
		State->DeferredDocumentOpens.erase(Pending);
		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Document.WorkspaceType);
		if (!Workspace) return false;

		if (FEditorDocumentTab* Existing = FindDocument(DocumentId))
			*Existing = std::move(Document);
		else
			State->Documents.push_back(std::move(Document));

		FEditorDocumentTab* OpenedDocument = FindDocument(DocumentId);
		if (!OpenedDocument) return false;
		Workspace->ActivateDocument(*OpenedDocument);
		State->ActiveDocumentId = OpenedDocument->Id;
		return true;
	}

	auto FEditorWorkspaceManager::OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool
	{
		const auto Found = State->AssetEditors.find(std::string(AssetClassName));
		if (Found == State->AssetEditors.end()) return false;

		const FEditorAssetEditorRegistration& AssetEditor = Found->second.Registration;
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
		if (!RequestDeactivateActiveDocument(DocumentId)) return false;
		Workspace->ActivateDocument(*Document);
		State->ActiveDocumentId = DocumentId;
		return true;
	}

	auto FEditorWorkspaceManager::ActivateWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) -> bool
	{
		if (const FEditorDocumentTab* ActiveDocument = GetActiveDocument(); ActiveDocument && ActiveDocument->WorkspaceType == WorkspaceType)
			return true;
		const auto Found = std::ranges::find(State->Documents, WorkspaceType, &FEditorDocumentTab::WorkspaceType);
		return Found != State->Documents.end() && ActivateDocument(Found->Id);
	}

	auto FEditorWorkspaceManager::OpenDefaultWorkspaces() -> bool
	{
		for (const FEditorWorkspaceDescriptor& Descriptor : GetWorkspaceDescriptors())
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

	auto FEditorWorkspaceManager::RequestCloseDocument(FEditorDocumentId DocumentId) -> EEditorDocumentCloseResult
	{
		if (State->PendingCloseDocumentId.IsValid())
		{
			return State->PendingCloseDocumentId == DocumentId
				? EEditorDocumentCloseResult::PendingConfirmation
				: EEditorDocumentCloseResult::Rejected;
		}
		if (State->DeferredDocumentOpens.erase(DocumentId.Value) != 0)
			return EEditorDocumentCloseResult::Closed;
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FEditorDocumentTab::Id);
		if (Found == State->Documents.end() || !Found->bClosable) return EEditorDocumentCloseResult::Rejected;
		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Found->WorkspaceType);
		if (!Workspace) return EEditorDocumentCloseResult::Rejected;
		const EEditorDocumentCloseResult CloseResult = Workspace->RequestCloseDocument(*Found);
		if (CloseResult == EEditorDocumentCloseResult::PendingConfirmation)
		{
			State->PendingCloseDocumentId = DocumentId;
			return CloseResult;
		}
		if (CloseResult == EEditorDocumentCloseResult::Rejected) return CloseResult;

		const size_t ClosedIndex = static_cast<size_t>(std::distance(State->Documents.begin(), Found));
		const bool bWasActive = State->ActiveDocumentId == DocumentId;
		State->Documents.erase(Found);
		if (!bWasActive) return EEditorDocumentCloseResult::Closed;

		State->ActiveDocumentId = {};
		if (State->Documents.empty()) return EEditorDocumentCloseResult::Closed;
		const size_t NextIndex = std::min(ClosedIndex, State->Documents.size() - 1);
		ActivateDocument(State->Documents[NextIndex].Id);
		return EEditorDocumentCloseResult::Closed;
	}

	auto FEditorWorkspaceManager::ResolvePendingDocumentClose(
		EEditorDocumentCloseResponse Response
	) -> EEditorDocumentCloseResult
	{
		FEditorDocumentTab* Document = FindDocument(State->PendingCloseDocumentId);
		if (!Document)
		{
			State->PendingCloseDocumentId = {};
			return EEditorDocumentCloseResult::Rejected;
		}
		if (Response == EEditorDocumentCloseResponse::Cancel)
		{
			State->PendingCloseDocumentId = {};
			return EEditorDocumentCloseResult::Cancelled;
		}

		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Document->WorkspaceType);
		if (!Workspace) return EEditorDocumentCloseResult::Rejected;
		const bool bResolved = Response == EEditorDocumentCloseResponse::Save
			? Workspace->SaveDocument(*Document)
			: Workspace->DiscardDocument(*Document);
		if (!bResolved) return EEditorDocumentCloseResult::PendingConfirmation;

		const FEditorDocumentId DocumentId = Document->Id;
		State->PendingCloseDocumentId = {};
		return RequestCloseDocument(DocumentId);
	}

	auto FEditorWorkspaceManager::RefreshDocumentState() -> void
	{
		for (FEditorDocumentTab& Document : State->Documents)
		{
			if (const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(Document.WorkspaceType))
				Document.bDirty = Workspace->IsDocumentDirty(Document);
		}
	}

	auto FEditorWorkspaceManager::GetDocuments() const -> const std::vector<FEditorDocumentTab>&
	{
		return State->Documents;
	}

	auto FEditorWorkspaceManager::GetActiveDocument() -> FEditorDocumentTab*
	{
		return FindDocument(State->ActiveDocumentId);
	}

	auto FEditorWorkspaceManager::GetActiveDocument() const -> const FEditorDocumentTab*
	{
		const auto Found = std::ranges::find(State->Documents, State->ActiveDocumentId, &FEditorDocumentTab::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorWorkspaceManager::GetPendingCloseDocument() const -> const FEditorDocumentTab*
	{
		const auto Found = std::ranges::find(State->Documents, State->PendingCloseDocumentId, &FEditorDocumentTab::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorWorkspaceManager::FindWorkspace(const FEditorWorkspaceTypeId& WorkspaceType) const -> std::shared_ptr<IEditorWorkspace>
	{
		const auto Found = State->Workspaces.find(std::string(WorkspaceType.GetValue()));
		return Found == State->Workspaces.end() ? nullptr : Found->second.Workspace;
	}

	auto FEditorWorkspaceManager::GetRegisteredWorkspaces() const -> std::vector<std::shared_ptr<IEditorWorkspace>>
	{
		std::vector<const Detail::FRegisteredWorkspace*> Entries;
		Entries.reserve(State->Workspaces.size());
		for (const auto& [Type, Entry] : State->Workspaces)
		{
			(void)Type;
			Entries.push_back(&Entry);
		}
		std::ranges::sort(Entries, {}, &Detail::FRegisteredWorkspace::RegistrationOrder);
		std::vector<std::shared_ptr<IEditorWorkspace>> Result;
		Result.reserve(Entries.size());
		for (const Detail::FRegisteredWorkspace* Entry : Entries) Result.push_back(Entry->Workspace);
		return Result;
	}

	auto FEditorWorkspaceManager::GetWorkspaceDescriptors() const -> std::vector<FEditorWorkspaceDescriptor>
	{
		std::vector<const Detail::FRegisteredWorkspace*> Entries;
		Entries.reserve(State->Workspaces.size());
		for (const auto& [Type, Entry] : State->Workspaces)
		{
			(void)Type;
			Entries.push_back(&Entry);
		}
		std::ranges::sort(Entries, {}, &Detail::FRegisteredWorkspace::RegistrationOrder);
		std::vector<FEditorWorkspaceDescriptor> Result;
		Result.reserve(Entries.size());
		for (const Detail::FRegisteredWorkspace* Entry : Entries) Result.push_back(Entry->Descriptor);
		return Result;
	}

	auto FEditorWorkspaceManager::FindDocument(FEditorDocumentId DocumentId) -> FEditorDocumentTab*
	{
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FEditorDocumentTab::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorWorkspaceManager::FindDocument(const FEditorWorkspaceTypeId& WorkspaceType, std::string_view DocumentKey) -> FEditorDocumentTab*
	{
		const auto Found = std::ranges::find_if(State->Documents, [&](const FEditorDocumentTab& Document) {
			return Document.WorkspaceType == WorkspaceType && Document.DocumentKey == DocumentKey;
		});
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorWorkspaceManager::RequestDeactivateActiveDocument(FEditorDocumentId NextDocumentId) -> bool
	{
		if (!State->ActiveDocumentId.IsValid() || State->ActiveDocumentId == NextDocumentId) return true;
		const FEditorDocumentTab* ActiveDocument = GetActiveDocument();
		if (!ActiveDocument) return true;
		const std::shared_ptr<IEditorWorkspace> Workspace = FindWorkspace(ActiveDocument->WorkspaceType);
		return !Workspace || Workspace->RequestDeactivate();
	}

	auto FEditorWorkspaceManager::AssetLabel(std::string_view ResourceId) -> std::string
	{
		const size_t Separator = ResourceId.find_last_of("/\\");
		const std::string_view Leaf = Separator == std::string_view::npos ? ResourceId : ResourceId.substr(Separator + 1);
		return Leaf.empty() ? std::string(ResourceId) : std::string(Leaf);
	}
}
