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
	// Toolkit-local collector roots do not impersonate externally indexed editor instances.
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
		std::vector<FEditorDocument> Documents;
		std::unordered_map<uint64, FEditorDocument> DeferredDocumentOpens;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditorSubsystem;
		std::unordered_map<uint64, std::shared_ptr<FDocumentAssetRoot>> AssetRoots;
		std::weak_ptr<IAssetEditorToolkit> Toolkit;
		bool bToolkitAttached = false;
		std::function<void()> AssetsChanged;
	};

	// Rebinds document roots atomically; workspace UI notification follows publication.
	class FDocumentReloadParticipant final : public IPackageReloadParticipant
	{
	public:
		explicit FDocumentReloadParticipant(std::shared_ptr<FEditorDocumentState> InState,
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
	{
	}

	FEditorDocumentManager::~FEditorDocumentManager()
	{
		State->AssetsChanged = {};
		State->AssetRoots.clear();
		Durin::OnAssetRenamed().Remove(AssetRenameHandle);
	}

	auto FEditorDocumentManager::RetireToolkit() -> void
	{
		State->Toolkit.reset();
		State->AssetsChanged = {};
		State->AssetRoots.clear();
		Durin::OnAssetRenamed().Remove(AssetRenameHandle);
		AssetRenameHandle = {};
	}

	auto FEditorDocumentManager::SynchronizeAssets() -> void
	{
		if (const auto Toolkit = std::dynamic_pointer_cast<FAssetEditorToolkit>(GetToolkit());
			Toolkit && Toolkit->GetOwnedEditingObjects())
		{
			State->AssetRoots.clear();
			if (State->AssetsChanged) State->AssetsChanged();
			return;
		}
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
		if (State->AssetsChanged) State->AssetsChanged();
	}

	auto FEditorDocumentManager::AttachToolkit(std::shared_ptr<IAssetEditorToolkit> Toolkit,
		std::function<void()> AssetsChanged, DAssetEditorSubsystem* AssetEditorSubsystem) -> bool
	{
		if (State->bToolkitAttached || !State->Documents.empty()
			|| !Toolkit || !Toolkit->GetToolkitId().IsValid()) return false;
		State->bToolkitAttached = true;
		State->AssetEditorSubsystem = AssetEditorSubsystem;
		State->Toolkit = Toolkit;
		State->AssetsChanged = std::move(AssetsChanged);
		if (!AssetEditorSubsystem)
			AssetRenameHandle = Durin::OnAssetRenamed().AddLambda([this](const FAssetData& Asset, const FSoftObjectPath& Old) {
				OnAssetRenamed(Asset, Old);
			});
		return true;
	}
	auto FEditorDocumentManager::AssociateDocumentAsset(FDocumentId Id, DObject* Asset) -> bool
	{
		auto* Document = FindDocument(Id);
		if (!Document || (Asset && !IsValid(Asset))) return false;
		if (!Asset)
		{
			State->AssetRoots.erase(Id.Value);
			if (State->AssetsChanged) State->AssetsChanged();
			return true;
		}
		Document->ResourceId = Asset->GetObjectPath();
		SynchronizeAssets();
		return true;
	}

	auto FEditorDocumentManager::GetEditedAssets() const -> std::vector<DObject*>
	{
		std::vector<DObject*> Assets;
		for (const auto& Document : State->Documents)
			if (const auto Root = State->AssetRoots.find(Document.Id.Value); Root != State->AssetRoots.end())
				if (auto* Object = Root->second->GetAsset(); Object && !std::ranges::contains(Assets, Object))
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
		return std::make_shared<DocumentPrivate::FDocumentReloadParticipant>(State, std::move(Features));
	}

	auto FEditorDocumentManager::OnAssetRenamed(const FAssetData& Asset, const FSoftObjectPath& Old) -> void
	{
		std::vector<std::pair<std::string, std::string>> Resources;
		const auto Collect = [&](const FEditorDocument& Document) {
			FSoftObjectPath Source, Destination;
			if (FSoftObjectPath::TryCreate(Document.ResourceId, Source) && Source.GetAssetPath() == Old.GetAssetPath()
				&& FSoftObjectPath::TryCreate(Asset.AssetPath, Source.GetSubobjectNames(), Destination))
				Resources.emplace_back(Document.ResourceId, Destination.ToString());
		};
		for (const auto& Document : State->Documents) Collect(Document);
		for (const auto& [Id, Document] : State->DeferredDocumentOpens) Collect(Document);
		for (const auto& [Source, Destination] : Resources) RemapResourceId(Source, Destination);
		// Only a moved package key may migrate non-asset workspace resources.
		if (!FindResidentPackage(Old.GetPackagePath())) RemapResourceId(Old.GetPackagePath().ToString(), Asset.AssetPath.GetPackagePath().ToString());
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
		auto* Editors = GetAssetEditorSubsystem();
		const auto Toolkit = std::dynamic_pointer_cast<FAssetEditorToolkit>(GetToolkit());
		if (Editors && Toolkit)
			if (const auto Pending = Editors->FindPendingClose(*Toolkit))
				return Pending->LegacyDocumentId == DocumentId.Value && !Pending->bResume
					? EDocumentCloseResult::PendingConfirmation : EDocumentCloseResult::Rejected;

		const auto Result = TryCloseDocument(DocumentId);
		if (Result != EDocumentCloseResult::PendingConfirmation) return Result;
		const auto* Document = FindDocument(DocumentId);
		if (!Editors || !Toolkit || !Document || !Toolkit->IsEditorOpen()) return EDocumentCloseResult::Rejected;
		Editors->QueueClose(*Toolkit, DocumentId.Value, Document->Label,
			[this, Requested = *Document](EDocumentCloseResponse Response) { return ResolveCloseDocument(Requested, Response); });
		return Result;
	}

	auto FEditorDocumentManager::TryCloseDocument(FDocumentId DocumentId) -> EDocumentCloseResult
	{
		if (State->DeferredDocumentOpens.erase(DocumentId.Value) != 0)
		{
			return EDocumentCloseResult::Closed;
		}
		const auto Found = std::ranges::find(State->Documents, DocumentId, &FEditorDocument::Id);
		if (Found == State->Documents.end() || !Found->bClosable) return EDocumentCloseResult::Rejected;
		const std::shared_ptr<IAssetEditorToolkit> Workspace = FindToolkit(Found->ToolkitId);
		if (!Workspace) return EDocumentCloseResult::Rejected;
		const FEditorDocument Snapshot = *Found;
		const EDocumentCloseResult CloseResult = Workspace->RequestCloseDocument(Snapshot);
		if (CloseResult != EDocumentCloseResult::Closed) return CloseResult;
		const auto Closed = std::ranges::find(State->Documents, DocumentId, &FEditorDocument::Id);
		if (Closed == State->Documents.end()) return EDocumentCloseResult::Closed;
		if (Closed->ResourceId != Snapshot.ResourceId) return EDocumentCloseResult::Rejected;

		const size_t ClosedIndex = static_cast<size_t>(std::distance(State->Documents.begin(), Closed));
		const bool bWasActive = State->ActiveDocumentId == DocumentId;
		State->Documents.erase(Closed);
		SynchronizeAssets();
		if (!bWasActive) return EDocumentCloseResult::Closed;

		State->ActiveDocumentId = {};
		if (State->Documents.empty()) return EDocumentCloseResult::Closed;
		const size_t NextIndex = std::min(ClosedIndex, State->Documents.size() - 1);
		ActivateDocument(State->Documents[NextIndex].Id);
		return EDocumentCloseResult::Closed;
	}

	auto FEditorDocumentManager::ResolvePendingDocumentClose(EDocumentCloseResponse Response) -> EDocumentCloseResult
	{
		auto* Editors = GetAssetEditorSubsystem();
		const auto Toolkit = std::dynamic_pointer_cast<FAssetEditorToolkit>(GetToolkit());
		const auto Pending = Editors && Toolkit ? Editors->FindPendingClose(*Toolkit) : nullptr;
		return Pending ? Editors->ResolvePendingClose(Pending->Prompt.RequestId, Response) : EDocumentCloseResult::Rejected;
	}

	auto FEditorDocumentManager::ResolveCloseDocument(const FEditorDocument& Requested, EDocumentCloseResponse Response) -> EDocumentCloseResult
	{
		const FDocumentId DocumentId = Requested.Id;
		const auto* Document = FindDocument(DocumentId);
		const auto Toolkit = std::dynamic_pointer_cast<FAssetEditorToolkit>(GetToolkit());
		if (!Document || Document->ResourceId != Requested.ResourceId || Document->DocumentKey != Requested.DocumentKey
			|| !Toolkit || !Toolkit->IsEditorOpen()) return EDocumentCloseResult::Rejected;
		const FEditorDocument Snapshot = *Document;
		const bool bResolved = Response == EDocumentCloseResponse::Save
			? Toolkit->SaveDocument(Snapshot) : Toolkit->DiscardDocument(Snapshot);
		const auto* Current = FindDocument(DocumentId);
		if (!Toolkit->IsEditorOpen() || !Current || Current->ResourceId != Snapshot.ResourceId)
			return Toolkit->IsClosed() ? EDocumentCloseResult::Closed : EDocumentCloseResult::Rejected;
		if (!bResolved) return EDocumentCloseResult::PendingConfirmation;
		const auto Result = TryCloseDocument(DocumentId);
		return Result == EDocumentCloseResult::Rejected ? EDocumentCloseResult::PendingConfirmation : Result;
	}

	auto FEditorDocumentManager::PrepareForExit() -> bool
	{
		if (GetPendingCloseSequence() != 0 || !State->DeferredDocumentOpens.empty()) return false;
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
		const auto* Editors = GetAssetEditorSubsystem();
		const auto Toolkit = std::dynamic_pointer_cast<FAssetEditorToolkit>(GetToolkit());
		const auto Pending = Editors && Toolkit ? Editors->FindPendingClose(*Toolkit) : nullptr;
		if (!Pending || Pending->bResume) return nullptr;
		const auto Found = std::ranges::find(State->Documents, FDocumentId{Pending->LegacyDocumentId}, &FEditorDocument::Id);
		return Found == State->Documents.end() ? nullptr : &*Found;
	}

	auto FEditorDocumentManager::GetPendingCloseSequence() const -> uint64
	{
		const auto* Editors = GetAssetEditorSubsystem();
		const auto Toolkit = std::dynamic_pointer_cast<FAssetEditorToolkit>(GetToolkit());
		const auto Pending = Editors && Toolkit ? Editors->FindPendingClose(*Toolkit) : nullptr;
		return Pending ? Pending->Prompt.RequestId : 0;
	}

	auto FEditorDocumentManager::FindToolkit(const FAssetEditorToolkitId& ToolkitId) const -> std::shared_ptr<IAssetEditorToolkit>
	{
		const auto Toolkit = GetToolkit();
		return Toolkit && Toolkit->GetToolkitId() == ToolkitId ? Toolkit : nullptr;
	}

	auto FEditorDocumentManager::GetToolkit() const -> std::shared_ptr<IAssetEditorToolkit>
	{ return State->Toolkit.lock(); }

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
