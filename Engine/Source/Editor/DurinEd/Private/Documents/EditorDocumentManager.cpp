#include "Documents/EditorDocumentManager.h"

#include "DObject/AssetPath.h"
#include "Asset/Load.h"
#include "Asset/PackageReload.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin::Editor::Detail
{
	// A document is one editor instance; the shared workspace is its presentation host.
	class FWorkspaceAssetEditorInstance final : public IAssetEditorInstance,
		public std::enable_shared_from_this<FWorkspaceAssetEditorInstance>
	{
	public:
		FWorkspaceAssetEditorInstance(FEditorDocumentManager& InManager, FDocumentId InId,
			FName InName, DAssetEditorSubsystem* InSubsystem)
			: Manager(InManager), Id(InId), Name(InName), Subsystem(InSubsystem) {}
		~FWorkspaceAssetEditorInstance() override
		{
			if (auto* Service = Subsystem.Get()) Service->NotifyEditorClosed(this);
		}
		auto GetEditorName() const -> FName override { return Name; }
		auto FocusWindow(DObject*) -> void override
		{
			const auto KeepAlive = shared_from_this();
			(void)Manager.ActivateDocument(Id);
			LastActivationTime = FTime::Seconds();
		}
		auto GetLastActivationTime() -> double override { return LastActivationTime; }
		auto RecordActivation() -> void { LastActivationTime = FTime::Seconds(); }
		auto CloseWindow(EAssetEditorCloseReason) -> bool override
		{
			const auto KeepAlive = shared_from_this();
			return Manager.RequestCloseDocument(Id) == EDocumentCloseResult::Closed;
		}
		auto SetAsset(DObject* Object) -> void
		{
			if (Asset.GetKey() != FObjectKey(Object)) Asset = FStrongObjectPtr(Object);
			if (RegisteredAsset == FObjectKey(Object)) return;
			if (auto* Service = Subsystem.Get())
			{
				Service->NotifyEditorClosed(this);
				if (Object) Service->NotifyAssetOpened(Object, this);
			}
			RegisteredAsset = FObjectKey(Object);
		}
		auto GetAsset() const -> DObject* { return Asset.Get(); }
		auto CommitReload(FStrongObjectPtr&& Replacement) noexcept -> void { Asset = std::move(Replacement); }
	private:
		FEditorDocumentManager& Manager;
		FDocumentId Id;
		FName Name;
		double LastActivationTime = FTime::Seconds();
		TWeakObjectPtr<DAssetEditorSubsystem> Subsystem;
		FStrongObjectPtr Asset;
		// Index notifications follow reload publication, outside its noexcept commit.
		FObjectKey RegisteredAsset;
	};
}

namespace Durin::Editor::Detail
{
	struct FRegisteredWorkspace
	{
		FAssetEditorToolkitDescriptor Descriptor;
		std::shared_ptr<IAssetEditorToolkit> Toolkit;
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
	struct FEditorDocumentRegistryState
	{
		uint64 NextRegistrationId = 1;
		uint64 NextWorkspaceRegistrationOrder = 1;
		uint64 NextDocumentId = 1;
		FDocumentId ActiveDocumentId;
		FDocumentId PendingCloseDocumentId;
		std::vector<FEditorDocument> Documents;
		std::unordered_map<uint64, FEditorDocument> DeferredDocumentOpens;
		std::unordered_map<std::string, FRegisteredWorkspace> Toolkits;
		std::unordered_map<std::string, FRegisteredAssetEditor> AssetEditors;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditorSubsystem;
		std::unordered_map<uint64, std::shared_ptr<FWorkspaceAssetEditorInstance>> EditorInstances;
		std::function<void()> SynchronizeAssetEditors;
	};

	// Rebinds document roots atomically; workspace UI notification follows publication.
	class FWorkspaceDocumentReloadParticipant final : public IPackageReloadParticipant
	{
	public:
		explicit FWorkspaceDocumentReloadParticipant(std::shared_ptr<FEditorDocumentRegistryState> InState)
			: State(std::move(InState)) {}
		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			Bindings.clear();
			Instances = State->EditorInstances;
			for (const auto& [Id, Instance] : Instances)
				if (const auto* Entry = Map.Find(Instance->GetAsset()))
				{
					if (!Entry->Replacement)
						return std::unexpected(FObjectReplacementError{
							.Code = EObjectReplacementError::UnmappedReference,
							.ObjectPath = Entry->Previous->GetObjectPath()});
					Bindings.push_back({Instance, FObjectKey(Entry->Previous), FStrongObjectPtr(Entry->Replacement)});
				}
			return {};
		}
		auto Validate() const -> bool override
		{
			if (Instances != State->EditorInstances) return false;
			return std::ranges::all_of(Bindings, [](const FBinding& Binding) {
				return FObjectKey(Binding.Instance->GetAsset()) == Binding.Previous;
			});
		}
		auto CoversNativeReferences(const DObject&) const -> bool override { return false; }
		auto GetStrongReferenceCount(const DObject& Object) const -> uint32 override
		{
			return static_cast<uint32>(std::ranges::count(Bindings, FObjectKey(&Object), &FBinding::Previous));
		}
		auto Commit() noexcept -> void override
		{
			for (auto& Binding : Bindings) Binding.Instance->CommitReload(std::move(Binding.Replacement));
		}
		auto Abort() noexcept -> void override { Bindings.clear(); Instances.clear(); }
		auto CanRetire() const -> bool override { return true; }
	private:
		struct FBinding
		{
			std::shared_ptr<FWorkspaceAssetEditorInstance> Instance;
			FObjectKey Previous;
			FStrongObjectPtr Replacement;
		};
		std::shared_ptr<FEditorDocumentRegistryState> State;
		std::unordered_map<uint64, std::shared_ptr<FWorkspaceAssetEditorInstance>> Instances;
		std::vector<FBinding> Bindings;
	};

	auto UnregisterWorkspaceBatch(FEditorDocumentRegistryState& State, uint64 RegistrationId) -> void
	{
		std::unordered_set<std::string> RemovedWorkspaceTypes;
		for (const auto& [ToolkitId, Entry] : State.Toolkits)
		{
			if (Entry.RegistrationId == RegistrationId) RemovedWorkspaceTypes.insert(ToolkitId);
		}

		const bool bActiveDocumentRemoved = std::ranges::any_of(State.Documents, [&](const FEditorDocument& Document) {
			return Document.Id == State.ActiveDocumentId && RemovedWorkspaceTypes.contains(std::string(Document.ToolkitId.GetValue()));
		});
		if (bActiveDocumentRemoved)
		{
			const auto Active = std::ranges::find(State.Documents, State.ActiveDocumentId, &FEditorDocument::Id);
			if (Active != State.Documents.end())
			{
				const auto Workspace = State.Toolkits.find(std::string(Active->ToolkitId.GetValue()));
				requiref(Workspace == State.Toolkits.end() || Workspace->second.Toolkit->RequestDeactivate(),
					"An editor workspace cannot be unloaded while an active property preview cannot be cancelled");
			}
		}
		// A module cannot leave documents backed by its code in the manager after unloading.
		std::erase_if(State.Documents, [&](const FEditorDocument& Document) {
			return RemovedWorkspaceTypes.contains(std::string(Document.ToolkitId.GetValue()));
		});
		if (const auto PendingClose = std::ranges::find(
				State.Documents, State.PendingCloseDocumentId, &FEditorDocument::Id
			); PendingClose == State.Documents.end())
			State.PendingCloseDocumentId = {};
		std::erase_if(State.DeferredDocumentOpens, [&](const auto& Pair) {
			return RemovedWorkspaceTypes.contains(std::string(Pair.second.ToolkitId.GetValue()));
		});
		std::erase_if(State.AssetEditors, [&](const auto& Pair) {
			return Pair.second.RegistrationId == RegistrationId ||
				RemovedWorkspaceTypes.contains(std::string(Pair.second.Registration.ToolkitId.GetValue()));
		});
		std::erase_if(State.Toolkits, [RegistrationId](const auto& Pair) {
			return Pair.second.RegistrationId == RegistrationId;
		});
		if (State.SynchronizeAssetEditors) State.SynchronizeAssetEditors();

		if (!bActiveDocumentRemoved) return;
		State.ActiveDocumentId = {};
		if (State.Documents.empty()) return;
		FEditorDocument& NextDocument = State.Documents.front();
		const auto Workspace = State.Toolkits.find(std::string(NextDocument.ToolkitId.GetValue()));
		if (Workspace == State.Toolkits.end()) return;
		State.ActiveDocumentId = NextDocument.Id;
		Workspace->second.Toolkit->ActivateDocument(NextDocument);
	}
}
namespace Durin::Editor
{
	FAssetEditorToolkitRegistrationHandle::FAssetEditorToolkitRegistrationHandle(
		std::weak_ptr<Detail::FEditorDocumentRegistryState> InState, uint64 InRegistrationId
	)
		: State(std::move(InState)), RegistrationId(InRegistrationId)
	{
	}

	FAssetEditorToolkitRegistrationHandle::~FAssetEditorToolkitRegistrationHandle()
	{
		Reset();
	}

	FAssetEditorToolkitRegistrationHandle::FAssetEditorToolkitRegistrationHandle(FAssetEditorToolkitRegistrationHandle&& Other) noexcept
		: State(std::move(Other.State)), RegistrationId(std::exchange(Other.RegistrationId, 0))
	{
	}

	auto FAssetEditorToolkitRegistrationHandle::operator=(FAssetEditorToolkitRegistrationHandle&& Other) noexcept -> FAssetEditorToolkitRegistrationHandle&
	{
		if (this == &Other) return *this;
		Reset();
		State = std::move(Other.State);
		RegistrationId = std::exchange(Other.RegistrationId, 0);
		return *this;
	}

	auto FAssetEditorToolkitRegistrationHandle::Reset() -> void
	{
		if (RegistrationId == 0) return;
		if (const std::shared_ptr<Detail::FEditorDocumentRegistryState> PinnedState = State.lock())
			Detail::UnregisterWorkspaceBatch(*PinnedState, RegistrationId);
		State.reset();
		RegistrationId = 0;
	}

	FEditorDocumentManager::FEditorDocumentManager(DAssetEditorSubsystem* AssetEditors)
		: State(std::make_shared<Detail::FEditorDocumentRegistryState>())
	{
		State->SynchronizeAssetEditors = [this] { SynchronizeAssetEditors(); };
		if (AssetEditors)
		{
			require(AssetEditors->CanCall());
			require(AssetEditors->DocumentManager == nullptr);
			AssetEditors->DocumentManager = this;
			State->AssetEditorSubsystem = AssetEditors;
		}
		AssetMoveObserverHandle = RegisterAssetMoveObserver(this);
	}

	FEditorDocumentManager::~FEditorDocumentManager()
	{
		State->SynchronizeAssetEditors = {};
		State->EditorInstances.clear();
		if (auto* Service = State->AssetEditorSubsystem.Get(); Service && Service->DocumentManager == this)
			Service->DocumentManager = nullptr;
		UnregisterAssetMoveObserver(AssetMoveObserverHandle);
	}

	auto FEditorDocumentManager::SynchronizeAssetEditors() -> void
	{
		auto* Service = State->AssetEditorSubsystem.Get();
		std::unordered_set<uint64> OpenDocuments;
		for (const auto& Document : State->Documents)
		{
			FObjectPath Path;
			if (!FObjectPath::TryCreate(Document.ResourceId, Path)) continue;
			FSoftObjectPtr Reference(Path);
			const auto Resolved = ResolveSoftObject(Reference, DObject::StaticClass());
			auto* Asset = Resolved ? Resolved->Object : nullptr;
			OpenDocuments.insert(Document.Id.Value);
			auto& Instance = State->EditorInstances[Document.Id.Value];
			if (!Instance) Instance = std::make_shared<Detail::FWorkspaceAssetEditorInstance>(
				*this, Document.Id, FName(Document.ToolkitId.GetValue()), Service);
			Instance->SetAsset(Asset);
		}
		std::erase_if(State->EditorInstances, [&](const auto& Pair) { return !OpenDocuments.contains(Pair.first); });
	}

	auto FEditorDocumentManager::CreateDocumentReloadParticipant() -> std::shared_ptr<IPackageReloadParticipant>
	{
		return std::make_shared<Detail::FWorkspaceDocumentReloadParticipant>(State);
	}

	auto FEditorDocumentManager::OnAssetsRelocated(
		std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		for (const FAssetRelocationMapping& Mapping : Mappings)
			RemapResourceId(
				Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
	}

	auto FEditorDocumentManager::RegisterBatch(
		FAssetEditorToolkitRegistrationBatch Batch) -> FAssetEditorToolkitRegistrationHandle
	{
		if (Batch.Toolkits.empty() && Batch.AssetEditors.empty()) return {};
		std::unordered_set<std::string> BatchWorkspaceTypes;
		std::unordered_set<std::string> BatchRootKeys;
		for (const FAssetEditorToolkitRegistration& Registration : Batch.Toolkits)
		{
			const FAssetEditorToolkitDescriptor& Descriptor = Registration.Descriptor;
			if (!Registration.Toolkit || !Descriptor.ToolkitId.IsValid() || Descriptor.DisplayName.empty() || Descriptor.RootKey.empty()) return {};
			if (Registration.Toolkit->GetToolkitId() != Descriptor.ToolkitId) return {};
			if (Descriptor.bOpenByDefault && !Descriptor.HasSingletonDocument()) return {};
			if (Descriptor.SingletonDocumentKey.empty() != Descriptor.SingletonDocumentLabel.empty()) return {};
			const std::string ToolkitId(Descriptor.ToolkitId.GetValue());
			if (State->Toolkits.contains(ToolkitId) || !BatchWorkspaceTypes.insert(ToolkitId).second) return {};
			for (const auto& [ExistingType, Existing] : State->Toolkits)
			{
				(void)ExistingType;
				if (Existing.Descriptor.RootKey == Descriptor.RootKey) return {};
			}
			if (!BatchRootKeys.insert(Descriptor.RootKey).second) return {};
		}

		std::unordered_set<std::string> BatchAssetClasses;
		for (const FAssetEditorRegistration& Registration : Batch.AssetEditors)
		{
			if (Registration.AssetClassName.empty() || !Registration.ToolkitId.IsValid()) return {};
			if (State->AssetEditors.contains(Registration.AssetClassName) || !BatchAssetClasses.insert(Registration.AssetClassName).second) return {};
			const std::string ToolkitId(Registration.ToolkitId.GetValue());
			if (!State->Toolkits.contains(ToolkitId) && !BatchWorkspaceTypes.contains(ToolkitId)) return {};
			if (Registration.DocumentPolicy == EDocumentPolicy::Singleton &&
				(Registration.SingletonDocumentKey.empty() || Registration.SingletonLabel.empty()))
				return {};
		}

		// Validation is deliberately complete before mutation, so a malformed later
		// entry cannot leave an earlier workspace or asset route installed.
		const uint64 RegistrationId = State->NextRegistrationId++;
		for (FAssetEditorToolkitRegistration& Registration : Batch.Toolkits)
		{
			const std::string ToolkitId(Registration.Descriptor.ToolkitId.GetValue());
			State->Toolkits.emplace(ToolkitId, Detail::FRegisteredWorkspace{
				.Descriptor = std::move(Registration.Descriptor),
				.Toolkit = std::move(Registration.Toolkit),
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
		return FAssetEditorToolkitRegistrationHandle(State, RegistrationId);
	}

	auto FEditorDocumentManager::OpenDocument(FDocumentRequest Request) -> FDocumentId
	{
		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Request.ToolkitId);
		if (!Workspace || Request.DocumentKey.empty() || Request.Label.empty()) return {};

		if (FEditorDocument* Existing = FindDocument(Request.ToolkitId, Request.DocumentKey))
		{
			const bool bReplacingActiveResource = State->ActiveDocumentId == Existing->Id
				&& Existing->ResourceId != Request.ResourceId;
			if (!RequestDeactivateActiveDocument(bReplacingActiveResource ? FDocumentId{} : Existing->Id)) return {};
			FEditorDocument Candidate = *Existing;
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
			SynchronizeAssetEditors();
			return Existing->Id;
		}

		FEditorDocument Document;
		Document.Id.Value = State->NextDocumentId++;
		Document.ToolkitId = std::move(Request.ToolkitId);
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
		SynchronizeAssetEditors();
		return State->Documents.back().Id;
	}

	auto FEditorDocumentManager::CompleteDeferredDocumentOpen(FDocumentId DocumentId, bool bSucceeded) -> bool
	{
		const auto Pending = State->DeferredDocumentOpens.find(DocumentId.Value);
		if (Pending == State->DeferredDocumentOpens.end()) return false;
		if (!bSucceeded)
		{
			State->DeferredDocumentOpens.erase(Pending);
			return true;
		}

		FEditorDocument Document = std::move(Pending->second);
		State->DeferredDocumentOpens.erase(Pending);
		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Document.ToolkitId);
		if (!Workspace) return false;

		if (FEditorDocument* Existing = FindDocument(DocumentId))
			*Existing = std::move(Document);
		else
			State->Documents.push_back(std::move(Document));

		FEditorDocument* OpenedDocument = FindDocument(DocumentId);
		if (!OpenedDocument) return false;
		Workspace->ActivateDocument(*OpenedDocument);
		State->ActiveDocumentId = OpenedDocument->Id;
		SynchronizeAssetEditors();
		return true;
	}

	auto FEditorDocumentManager::CanOpenAsset(std::string_view AssetClassName) const -> bool
	{
		return State->AssetEditors.contains(std::string(AssetClassName));
	}

	auto FEditorDocumentManager::OpenAsset(std::string ResourceId, std::string_view AssetClassName) -> bool
	{
		const auto Found = State->AssetEditors.find(std::string(AssetClassName));
		if (Found == State->AssetEditors.end()) return false;

		const FAssetEditorRegistration& AssetEditor = Found->second.Registration;
		FDocumentRequest Request;
		Request.ToolkitId = AssetEditor.ToolkitId;
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

	auto FEditorDocumentManager::ActivateDocument(FDocumentId DocumentId) -> bool
	{
		FEditorDocument* Document = FindDocument(DocumentId);
		if (!Document) return false;
		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Document->ToolkitId);
		if (!Workspace) return false;
		if (!RequestDeactivateActiveDocument(DocumentId)) return false;
		Workspace->ActivateDocument(*Document);
		State->ActiveDocumentId = DocumentId;
		if (const auto Instance = State->EditorInstances.find(DocumentId.Value); Instance != State->EditorInstances.end())
			Instance->second->RecordActivation();
		return true;
	}

	auto FEditorDocumentManager::ActivateToolkit(const FAssetEditorToolkitId& ToolkitId) -> bool
	{
		if (const FEditorDocument* ActiveDocument = GetActiveDocument(); ActiveDocument && ActiveDocument->ToolkitId == ToolkitId)
			return true;
		const auto Found = std::ranges::find(State->Documents, ToolkitId, &FEditorDocument::ToolkitId);
		return Found != State->Documents.end() && ActivateDocument(Found->Id);
	}

	auto FEditorDocumentManager::OpenDefaultToolkits() -> bool
	{
		for (const FAssetEditorToolkitDescriptor& Descriptor : GetToolkitDescriptors())
		{
			if (!Descriptor.bOpenByDefault) continue;
			if (!Descriptor.HasSingletonDocument()) return false;
			if (!OpenDocument({
				.ToolkitId = Descriptor.ToolkitId,
				.DocumentKey = Descriptor.SingletonDocumentKey,
				.Label = Descriptor.SingletonDocumentLabel,
				.bClosable = Descriptor.bSingletonDocumentClosable,
			}).IsValid()) return false;
		}
		return true;
	}

	auto FEditorDocumentManager::RequestCloseDocument(FDocumentId DocumentId) -> EDocumentCloseResult
	{
		if (State->PendingCloseDocumentId.IsValid())
		{
			return State->PendingCloseDocumentId == DocumentId
				? EDocumentCloseResult::PendingConfirmation
				: EDocumentCloseResult::Rejected;
		}
		if (State->DeferredDocumentOpens.erase(DocumentId.Value) != 0)
			return EDocumentCloseResult::Closed;
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FEditorDocument::Id);
		if (Found == State->Documents.end() || !Found->bClosable) return EDocumentCloseResult::Rejected;
		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Found->ToolkitId);
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
		SynchronizeAssetEditors();
		if (!bWasActive) return EDocumentCloseResult::Closed;

		State->ActiveDocumentId = {};
		if (State->Documents.empty()) return EDocumentCloseResult::Closed;
		const size_t NextIndex = std::min(ClosedIndex, State->Documents.size() - 1);
		ActivateDocument(State->Documents[NextIndex].Id);
		return EDocumentCloseResult::Closed;
	}

	auto FEditorDocumentManager::ResolvePendingDocumentClose(
		EDocumentCloseResponse Response
	) -> EDocumentCloseResult
	{
		FEditorDocument* Document = FindDocument(State->PendingCloseDocumentId);
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

		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Document->ToolkitId);
		if (!Workspace) return EDocumentCloseResult::Rejected;
		const bool bResolved = Response == EDocumentCloseResponse::Save
			? Workspace->SaveDocument(*Document)
			: Workspace->DiscardDocument(*Document);
		if (!bResolved) return EDocumentCloseResult::PendingConfirmation;

		const FDocumentId DocumentId = Document->Id;
		State->PendingCloseDocumentId = {};
		return RequestCloseDocument(DocumentId);
	}

	auto FEditorDocumentManager::PrepareForExit() -> bool
	{
		if (State->PendingCloseDocumentId.IsValid() || !State->DeferredDocumentOpens.empty()) return false;
		for (const auto& Workspace : GetRegisteredToolkits())
			if (!Workspace->RequestDeactivate()) return false;
		RefreshDocumentState();
		return true;
	}

	auto FEditorDocumentManager::SaveDocumentsForExit() -> bool
	{
		if (!PrepareForExit()) return false;
		// Persistence callbacks may update document routing; never borrow vector elements across them.
		const auto Documents = State->Documents;
		for (const FEditorDocument& Document : Documents)
		{
			const auto Workspace = FindToolkit(Document.ToolkitId);
			if (!Workspace) return false;
			if (Workspace->IsDocumentDirty(Document) && !Workspace->SaveDocument(Document)) return false;
		}
		if (!PrepareForExit()) return false;
		return std::ranges::none_of(State->Documents, &FEditorDocument::bDirty);
	}

	auto FEditorDocumentManager::RefreshDocumentState() -> void
	{
		for (FEditorDocument& Document : State->Documents)
		{
			if (const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Document.ToolkitId))
				Document.bDirty = Workspace->IsDocumentDirty(Document);
		}
	}

	auto FEditorDocumentManager::NotifyPackageReloaded(
		DPackage* Previous, DPackage* Replacement) -> void
	{
		if (!Previous || !Replacement || Previous == Replacement) return;
		for (const auto& Workspace : GetRegisteredToolkits())
			Workspace->OnPackageReloaded(Previous, Replacement);
		SynchronizeAssetEditors();
	}

	auto FEditorDocumentManager::RemapResourceId(
		std::string_view SourceResourceId,
		std::string_view DestinationResourceId) -> void
	{
		if (SourceResourceId.empty() || DestinationResourceId.empty()
			|| SourceResourceId == DestinationResourceId) return;
		const auto Remap = [&](FEditorDocument& Document) {
			if (Document.ResourceId != SourceResourceId) return;
			Document.ResourceId = DestinationResourceId;
			if (Document.DocumentKey == SourceResourceId)
				Document.DocumentKey = DestinationResourceId;
			Document.Label = AssetLabel(DestinationResourceId);
		};
		for (FEditorDocument& Document : State->Documents) Remap(Document);
		for (auto& [Id, Document] : State->DeferredDocumentOpens)
		{
			(void)Id;
			Remap(Document);
		}
		SynchronizeAssetEditors();
	}

	auto FEditorDocumentManager::GetDocuments() const -> const std::vector<FEditorDocument>&
	{
		return State->Documents;
	}

	auto FEditorDocumentManager::GetActiveDocument() -> FEditorDocument*
	{
		return FindDocument(State->ActiveDocumentId);
	}

	auto FEditorDocumentManager::GetActiveDocument() const -> const FEditorDocument*
	{
		const auto Found = std::ranges::find(State->Documents, State->ActiveDocumentId, &FEditorDocument::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorDocumentManager::GetPendingCloseDocument() const -> const FEditorDocument*
	{
		const auto Found = std::ranges::find(State->Documents, State->PendingCloseDocumentId, &FEditorDocument::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorDocumentManager::FindToolkit(const FAssetEditorToolkitId& ToolkitId) const -> std::shared_ptr<IAssetEditorToolkit>
	{
		const auto Found = State->Toolkits.find(std::string(ToolkitId.GetValue()));
		return Found == State->Toolkits.end() ? nullptr : Found->second.Toolkit;
	}

	auto FEditorDocumentManager::GetRegisteredToolkits() const -> std::vector<std::shared_ptr<IAssetEditorToolkit>>
	{
		std::vector<const Detail::FRegisteredWorkspace*> Entries;
		Entries.reserve(State->Toolkits.size());
		for (const auto& [Type, Entry] : State->Toolkits)
		{
			(void)Type;
			Entries.push_back(&Entry);
		}
		std::ranges::sort(Entries, {}, &Detail::FRegisteredWorkspace::RegistrationOrder);
		std::vector<std::shared_ptr<IAssetEditorToolkit>> Result;
		Result.reserve(Entries.size());
		for (const Detail::FRegisteredWorkspace* Entry : Entries) Result.push_back(Entry->Toolkit);
		return Result;
	}

	auto FEditorDocumentManager::GetToolkitDescriptors() const -> std::vector<FAssetEditorToolkitDescriptor>
	{
		std::vector<const Detail::FRegisteredWorkspace*> Entries;
		Entries.reserve(State->Toolkits.size());
		for (const auto& [Type, Entry] : State->Toolkits)
		{
			(void)Type;
			Entries.push_back(&Entry);
		}
		std::ranges::sort(Entries, {}, &Detail::FRegisteredWorkspace::RegistrationOrder);
		std::vector<FAssetEditorToolkitDescriptor> Result;
		Result.reserve(Entries.size());
		for (const Detail::FRegisteredWorkspace* Entry : Entries) Result.push_back(Entry->Descriptor);
		return Result;
	}

	auto FEditorDocumentManager::FindDocument(FDocumentId DocumentId) -> FEditorDocument*
	{
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FEditorDocument::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorDocumentManager::FindDocument(const FAssetEditorToolkitId& ToolkitId, std::string_view DocumentKey) -> FEditorDocument*
	{
		const auto Found = std::ranges::find_if(State->Documents, [&](const FEditorDocument& Document) {
			return Document.ToolkitId == ToolkitId && Document.DocumentKey == DocumentKey;
		});
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorDocumentManager::RequestDeactivateActiveDocument(FDocumentId NextDocumentId) -> bool
	{
		if (!State->ActiveDocumentId.IsValid() || State->ActiveDocumentId == NextDocumentId) return true;
		const FEditorDocument* ActiveDocument = GetActiveDocument();
		if (!ActiveDocument) return true;
		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(ActiveDocument->ToolkitId);
		return !Workspace || Workspace->RequestDeactivate();
	}

	auto FEditorDocumentManager::AssetLabel(std::string_view ResourceId) -> std::string
	{
		FTopLevelAssetPath AssetPath;
		if (FTopLevelAssetPath::TryCreate(ResourceId, AssetPath))
			return std::string(AssetPath.GetAssetName());

		const size_t Separator = ResourceId.find_last_of("/\\");
		const std::string_view Leaf = Separator == std::string_view::npos ? ResourceId : ResourceId.substr(Separator + 1);
		return Leaf.empty() ? std::string(ResourceId) : std::string(Leaf);
	}
}
