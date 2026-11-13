#include "Documents/EditorDocumentManager.h"

#include "DObject/AssetPath.h"
#include "Asset/Load.h"
#include "Asset/PackageReload.h"
#include "DObject/SoftObjectPtr.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/WeakObjectPtr.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace Durin::DocumentPrivate
{
	// Session-local collector roots do not impersonate externally indexed editor instances.
	class FDocumentAssetRoot
	{
	public:
		auto SetAsset(DObject* Object) -> void
		{ if (Asset.GetKey() != FObjectKey(Object)) Asset = FStrongObjectPtr(Object); }
		auto GetAsset() const -> DObject* { return Asset.Get(); }
		auto CommitReload(FStrongObjectPtr&& Replacement) noexcept -> void { Asset = std::move(Replacement); }
	private:
		FStrongObjectPtr Asset;
	};

	// Owns one toolkit's document state and collector roots.
	struct FEditorDocumentState
	{
		FDocumentId ActiveDocumentId;
		FDocumentId PendingCloseDocumentId;
		uint64 PendingCloseSequence = 0;
		uint64 CloseCancellationRevision = 0;
		std::vector<FEditorDocument> Documents;
		std::unordered_map<uint64, FEditorDocument> DeferredDocumentOpens;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditorSubsystem;
		std::unordered_map<uint64, std::shared_ptr<FDocumentAssetRoot>> AssetRoots;
		std::weak_ptr<IAssetEditorToolkit> SessionToolkit;
		bool bToolkitAttached = false;
		std::function<void()> SessionAssetsChanged;
	};

	// Rebinds document roots atomically; workspace UI notification follows publication.
	class FSessionDocumentReloadParticipant final : public IPackageReloadParticipant
	{
	public:
		explicit FSessionDocumentReloadParticipant(std::shared_ptr<FEditorDocumentState> InState,
			std::vector<std::shared_ptr<IObjectReplacementParticipant>> InFeatures)
			: State(std::move(InState)), Features(std::move(InFeatures)) {}
		auto Prepare(const FObjectReplacementMap& Map) -> std::expected<void, FObjectReplacementError> override
		{
			Bindings.clear();
			Instances = State->AssetRoots;
			for (const auto& [Id, Instance] : Instances)
				if (const auto* Entry = Map.Find(Instance->GetAsset()))
				{
					if (!Entry->Replacement)
						return std::unexpected(FObjectReplacementError{
							.Code = EObjectReplacementError::UnmappedReference,
							.ObjectPath = Entry->Previous->GetObjectPath()});
					Bindings.push_back({Instance, FObjectKey(Entry->Previous), FStrongObjectPtr(Entry->Replacement)});
				}
			for (const auto& Feature : Features)
				if (auto Prepared = Feature->Prepare(Map); !Prepared) return Prepared;
			return {};
		}
		auto Validate() const -> bool override
		{
			if (Instances != State->AssetRoots || !std::ranges::all_of(Features, [](const auto& Feature) { return Feature->Validate(); })) return false;
			return std::ranges::all_of(Bindings, [](const FBinding& Binding) {
				return FObjectKey(Binding.Instance->GetAsset()) == Binding.Previous;
			});
		}
		auto CoversNativeReferences(const DObject& Owner) const -> bool override
		{ return std::ranges::any_of(Features, [&](const auto& Feature) { return Feature->CoversNativeReferences(Owner); }); }
		auto GetStrongReferenceCount(const DObject& Object) const -> uint32 override
		{
			uint32 Count = static_cast<uint32>(std::ranges::count(Bindings, FObjectKey(&Object), &FBinding::Previous));
			for (const auto& Feature : Features) Count += Feature->GetStrongReferenceCount(Object);
			return Count;
		}
		auto Commit() noexcept -> void override
		{
			for (auto& Binding : Bindings) Binding.Instance->CommitReload(std::move(Binding.Replacement));
			for (const auto& Feature : Features) Feature->Commit();
		}
		auto Abort() noexcept -> void override { Bindings.clear(); Instances.clear(); for (const auto& Feature : Features) Feature->Abort(); }
		auto CanRetire() const -> bool override
		{ return std::ranges::all_of(Features, [](const auto& Feature) { return Feature->CanRetire(); }); }
	private:
		struct FBinding
		{
			std::shared_ptr<FDocumentAssetRoot> Instance;
			FObjectKey Previous;
			FStrongObjectPtr Replacement;
		};
		std::shared_ptr<FEditorDocumentState> State;
		std::unordered_map<uint64, std::shared_ptr<FDocumentAssetRoot>> Instances;
		std::vector<FBinding> Bindings;
		std::vector<std::shared_ptr<IObjectReplacementParticipant>> Features;
	};

}
namespace Durin
{

	FEditorDocumentManager::FEditorDocumentManager()
		: State(std::make_shared<DocumentPrivate::FEditorDocumentState>())
	{ AssetMoveObserverHandle = RegisterAssetMoveObserver(this); }

	FEditorDocumentManager::~FEditorDocumentManager()
	{
		State->SessionAssetsChanged = {};
		State->AssetRoots.clear();
		UnregisterAssetMoveObserver(AssetMoveObserverHandle);
	}

	auto FEditorDocumentManager::SynchronizeAssets() -> void
	{
		std::unordered_set<uint64> OpenDocuments;
		for (const auto& Document : State->Documents)
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Document.ResourceId, Path)) continue;
			FSoftObjectPtr Reference(Path);
			auto* Asset = Reference.Get();
			OpenDocuments.insert(Document.Id.Value);
			auto& Instance = State->AssetRoots[Document.Id.Value];
			if (!Instance) Instance = std::make_shared<DocumentPrivate::FDocumentAssetRoot>();
			Instance->SetAsset(Asset);
		}
		std::erase_if(State->AssetRoots, [&](const auto& Pair) { return !OpenDocuments.contains(Pair.first); });
		if (State->SessionAssetsChanged) State->SessionAssetsChanged();
	}

	auto FEditorDocumentManager::AttachSessionToolkit(std::shared_ptr<IAssetEditorToolkit> Toolkit,
		std::function<void()> AssetsChanged, DAssetEditorSubsystem* Service) -> bool
	{
		if (State->bToolkitAttached || !State->Documents.empty()
			|| !Toolkit || !Toolkit->GetToolkitId().IsValid()) return false;
		State->bToolkitAttached = true;
		State->AssetEditorSubsystem = Service;
		State->SessionToolkit = Toolkit;
		State->SessionAssetsChanged = std::move(AssetsChanged);
		return true;
	}
	auto FEditorDocumentManager::AssociateDocumentAsset(FDocumentId Id, DObject* Asset) -> bool
	{
		auto* Document = FindDocument(Id);
		if (!Document || !IsValid(Asset)) return false;
		Document->ResourceId = Asset->GetObjectPath();
		SynchronizeAssets();
		return true;
	}

	auto FEditorDocumentManager::GetEditedAssets() const -> std::vector<DObject*>
	{
		std::vector<DObject*> Assets;
		for (const auto& [Id, Root] : State->AssetRoots)
			if (auto* Object = Root->GetAsset(); Object && std::ranges::find(Assets, Object) == Assets.end())
				Assets.push_back(Object);
		return Assets;
	}

	auto FEditorDocumentManager::GetAssetEditorSubsystem() const -> DAssetEditorSubsystem*
	{
		return State->AssetEditorSubsystem.Get();
	}

	auto FEditorDocumentManager::CreateDocumentReloadParticipant() -> std::shared_ptr<IPackageReloadParticipant>
	{
		std::vector<std::shared_ptr<IObjectReplacementParticipant>> Features;
		if (const auto Toolkit = GetToolkit())
			if (auto Feature = Toolkit->CreateReloadParticipant()) Features.push_back(std::move(Feature));
		return std::make_shared<DocumentPrivate::FSessionDocumentReloadParticipant>(State, std::move(Features));
	}

	auto FEditorDocumentManager::OnAssetsRelocated(
		std::span<const FAssetRelocationMapping> Mappings) -> void
	{
		for (const FAssetRelocationMapping& Mapping : Mappings)
		{
			std::vector<std::pair<std::string, std::string>> Resources;
			const auto Collect = [&](const FEditorDocument& Document) {
				FSoftObjectPath Source;
				if (!FSoftObjectPath::TryCreate(Document.ResourceId, Source) || Source.GetPackagePath() != Mapping.SourcePath) return;
				FTopLevelAssetPath DestinationAsset;
				FSoftObjectPath Destination;
				if (FTopLevelAssetPath::TryCreate(Mapping.DestinationPath, Source.GetAssetPath().GetAssetName(), DestinationAsset)
					&& FSoftObjectPath::TryCreate(DestinationAsset, Source.GetSubobjectNames(), Destination))
					Resources.emplace_back(Document.ResourceId, Destination.ToString());
			};
			for (const auto& Document : State->Documents) Collect(Document);
			for (const auto& [Id, Document] : State->DeferredDocumentOpens) Collect(Document);
			for (const auto& [Source, Destination] : Resources) RemapResourceId(Source, Destination);
			// Non-asset workspace documents may still use package-only resource keys.
			RemapResourceId(
				Mapping.SourcePath.ToString(), Mapping.DestinationPath.ToString());
		}
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
			SynchronizeAssets();
			return Existing->Id;
		}

		FEditorDocument Document;
		// Preview/render resources consume live document IDs across controllers.
		// Persistent layout identity remains based on canonical assets and stable roles.
		static uint64 NextDocumentId = 1;
		require(NextDocumentId != std::numeric_limits<uint64>::max());
		Document.Id.Value = NextDocumentId++;
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
		SynchronizeAssets();
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
		if (!Workspace)
		{
			return false;
		}

		if (FEditorDocument* Existing = FindDocument(DocumentId))
			*Existing = std::move(Document);
		else
			State->Documents.push_back(std::move(Document));

		FEditorDocument* OpenedDocument = FindDocument(DocumentId);
		if (!OpenedDocument) return false;
		Workspace->ActivateDocument(*OpenedDocument);
		State->ActiveDocumentId = OpenedDocument->Id;
		SynchronizeAssets();
		return true;
	}

	auto FEditorDocumentManager::IsDocumentOpenPending(std::string_view ResourceId) const -> bool
	{
		return std::ranges::any_of(State->DeferredDocumentOpens, [&](const auto& Entry) { return Entry.second.ResourceId == ResourceId; });
	}
	auto FEditorDocumentManager::IsDocumentOpenPending(FDocumentId DocumentId) const -> bool
	{
		return State->DeferredDocumentOpens.contains(DocumentId.Value);
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
		return true;
	}

	auto FEditorDocumentManager::RequestDeactivateActiveToolkit() -> bool
	{
		return RequestDeactivateActiveDocument();
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
		{
			return EDocumentCloseResult::Closed;
		}
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FEditorDocument::Id);
		if (Found == State->Documents.end() || !Found->bClosable) return EDocumentCloseResult::Rejected;
		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Found->ToolkitId);
		if (!Workspace) return EDocumentCloseResult::Rejected;
		const EDocumentCloseResult CloseResult = Workspace->RequestCloseDocument(*Found);
		if (CloseResult == EDocumentCloseResult::PendingConfirmation)
		{
			State->PendingCloseDocumentId = DocumentId;
			static uint64 NextCloseSequence = 1;
			State->PendingCloseSequence = NextCloseSequence++;
			return CloseResult;
		}
		if (CloseResult == EDocumentCloseResult::Rejected) return CloseResult;

		const size_t ClosedIndex = static_cast<size_t>(std::distance(State->Documents.begin(), Found));
		const bool bWasActive = State->ActiveDocumentId == DocumentId;
		State->Documents.erase(Found);
		SynchronizeAssets();
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
			++State->CloseCancellationRevision;
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
		if (const auto Workspace = GetToolkit())
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
		if (const auto Workspace = GetToolkit())
			Workspace->OnPackageReloaded(Previous, Replacement);
		SynchronizeAssets();
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
		if (const auto Toolkit = GetToolkit()) Toolkit->OnResourceIdRemapped(SourceResourceId, DestinationResourceId);
		SynchronizeAssets();
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

	auto FEditorDocumentManager::GetPendingCloseSequence() const -> uint64
	{
		return GetPendingCloseDocument() ? State->PendingCloseSequence : 0;
	}
	auto FEditorDocumentManager::GetCloseCancellationRevision() const -> uint64 { return State->CloseCancellationRevision; }

	auto FEditorDocumentManager::FindToolkit(const FAssetEditorToolkitId& ToolkitId) const -> std::shared_ptr<IAssetEditorToolkit>
	{
		const auto Toolkit = GetToolkit();
		return Toolkit && Toolkit->GetToolkitId() == ToolkitId ? Toolkit : nullptr;
	}

	auto FEditorDocumentManager::GetToolkit() const -> std::shared_ptr<IAssetEditorToolkit>
	{ return State->SessionToolkit.lock(); }

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
