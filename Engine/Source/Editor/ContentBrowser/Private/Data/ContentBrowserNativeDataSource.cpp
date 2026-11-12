#include "ContentBrowserNativeDataSource.h"
#include "Data/ContentBrowserRequestContext.h"
#include "Data/ContentBrowserDataQuery.h"
#include "Asset/RegistryOperations.h"
#ifdef _WIN32
	#include <shellapi.h>
#endif
#include "Operations/ContentBrowserPaths.h"
#include "Misc/MountPaths.h"
#include "Editor/EditorEngine.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Package.h"
#include "Asset/Load.h"
namespace Durin
{
	using namespace ContentBrowser;
	namespace
	{
		using FOperations = ContentBrowser::Private::FContentBrowserRequestContext;
		auto ResolveItemPath(FContentBrowserItem& Item) -> void
		{
			if (Item.Kind != EContentBrowserItemKind::File && Item.Kind != EContentBrowserItemKind::Folder) return;
			const auto Path = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(Item.PhysicalPath);
			if (Path) Item.VirtualPath = Path.VirtualPath;
		}
		auto QueueCommittedUpdates(DContentBrowserDataSource& Source, const FContentChangeBatch& Batch) -> void
		{
			auto MakeData = [&](const FContentChange& Change, bool Old) {
				FContentBrowserItem Item;
				Item.PhysicalPath = Old ? Change.OldPhysicalPath : Change.NewPhysicalPath;
				const auto& AssetPath = Old ? Change.OldAssetPath : Change.NewAssetPath;
				Item.Kind = Change.bDirectory ? EContentBrowserItemKind::Folder : EContentBrowserItemKind::File;
				if (!AssetPath.empty() && !Change.bDirectory)
				{
					Item.Kind = EContentBrowserItemKind::Asset;
					Item.VirtualPath = AssetPath;
					FTopLevelAssetPath Path;
					if (FTopLevelAssetPath::TryCreate(AssetPath, Path)) Item.PackagePath = Path.GetPackagePath();
				}
				ResolveItemPath(Item);
				auto Data = Source.CreateItemData(Item);
				if (!Data.GetOwnerDataSource())
					if (auto* Service = Cast<DContentBrowserDataSubsystem>(Source.GetOuter()))
						if (auto* Peer = Service->GetActiveDataSource(FName(Item.Kind == EContentBrowserItemKind::File ? "File" : "Asset"))) Data = Peer->CreateItemData(Item);

				return Data;
			};
			auto Queue = [](FContentBrowserItemDataUpdate Update) {
				if (auto* Owner = Update.GetItemData().GetOwnerDataSource()) Owner->QueueItemDataUpdate(std::move(Update));
			};

			for (const auto& Change : Batch.Changes)
			{
				if (Change.Kind == EContentChangeKind::Removed)
					Queue(FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(MakeData(Change, true)));
				else if (Change.Kind == EContentChangeKind::Added)
					Queue(FContentBrowserItemDataUpdate::MakeItemAddedUpdate(MakeData(Change, false)));
				else if (Change.Kind == EContentChangeKind::Renamed)
				{
					auto Previous = MakeData(Change, true).GetVirtualPath();
					Queue(FContentBrowserItemDataUpdate::MakeItemMovedUpdate(MakeData(Change, false), std::move(Previous)));
				}
				else
					Queue(FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(MakeData(Change, false)));
			}
			if (Batch.bFullRefresh)
				for (const auto& Mount : ContentBrowser::Private::FContentBrowserPaths::CaptureMounts())
				{
					FContentBrowserItem Folder{.Kind = EContentBrowserItemKind::Folder, .VirtualPath = Mount.VirtualRoot, .PhysicalPath = Mount.PhysicalRoot};
					Queue(FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(Source.CreateItemData(Folder)));
				}
		}
		auto ScopedHost(DContentBrowserDataSource& Source, FContentBrowserHostContext Host) -> FContentBrowserHostContext
		{
			const TWeakObjectPtr<DContentBrowserDataSource> Weak(&Source);
			const auto Generation = Source.GetActivationGeneration();
			const auto MountRevision = FMountPaths::GetMountRegistryRevision();
			Host.NotifyScopedContentMutation = [Weak, Generation, MountRevision, Notify = Host.NotifyScopedContentMutation, Fallback = Host.NotifyMountedContentMutation](FContentChangeBatch Batch) {
				if (auto* Owner = Weak.Get(); Owner && Owner->IsActive() && Owner->GetActivationGeneration() == Generation
											  && FMountPaths::GetMountRegistryRevision() == MountRevision) QueueCommittedUpdates(*Owner, Batch);
				if (Notify)
					Notify(std::move(Batch));
				else if (Fallback)
					Fallback();
			};
			Host.NotifyMountedContentMutation = {};
			Host.CanMutateContent = [Weak, Generation, MountRevision, Allow = Host.CanMutateContent] {
				auto Admitted = [&] {
					auto* Owner = Weak.Get();
					if (!Owner) return false;
					for (auto* Outer = Owner->GetOuter(); Outer; Outer = Outer->GetOuter())
						if (auto* Editor = Cast<DEditorEngine>(Outer); Editor && !Editor->GetCapabilities().bEditWorld) return false;
					return Owner && Owner->IsActive() && Owner->GetActivationGeneration() == Generation
						   && FMountPaths::GetMountRegistryRevision() == MountRevision;
				};
				if (!Admitted() || (Allow && !Allow())) return false;
				return Admitted();
			};
			return Host;
		}
		struct FNativeItemPayload final : IContentBrowserItemDataPayload
		{
			FContentBrowserItem Item;
			// Immutable request policy belongs to the item; execution state belongs to a command.
			std::shared_ptr<const FContentBrowserHostContext> Host;
		};
		struct FNativeDeleteRequest final : IContentBrowserDeleteRequest
		{
			std::shared_ptr<FOperations> Operations;
			ContentBrowser::Private::FContentDeletionPlanPtr Plan;
			std::vector<std::shared_ptr<const IContentBrowserItemDataPayload>> Payloads;
			mutable ContentBrowser::Private::FContentDeletionPlanPtr PreviewPlan;
			mutable std::shared_ptr<const FContentBrowserDeletePreview> Preview;
			auto GetPreview() const -> std::shared_ptr<const FContentBrowserDeletePreview> override
			{
				if (Plan != PreviewPlan)
				{
					PreviewPlan = Plan;
					Preview.reset();
					if (Plan)
					{
						auto Next = std::make_shared<FContentBrowserDeletePreview>();
						Next->DisplayName = Plan->DisplayName;
						Next->Summary = {Plan->Summary.AssetCount, Plan->Summary.FileCount, Plan->Summary.CompanionCount, Plan->Summary.FolderCount};
						for (const auto& B : Plan->Blockers)
							Next->Blockers.push_back({B.DisplayName, B.PhysicalPath, B.RelatedAssetPath, B.Details});
						for (const auto& W : Plan->Warnings)
							Next->Warnings.push_back({W.DisplayName, {}, {}, W.Details});
						Preview = std::move(Next);
					}
				}
				return Preview;
			}
			~FNativeDeleteRequest() override
			{
				if (Operations && Plan) Operations->DismissDeletion(Plan);
			}
			auto CanExecute() const -> bool override { return Plan && Plan->CanExecute(); }
		};
		auto Payload(const DContentBrowserDataSource& Source, const FContentBrowserItemData& Data) -> std::shared_ptr<const FNativeItemPayload>
		{
			return Data.GetOwnerDataSource() == &Source ? std::dynamic_pointer_cast<const FNativeItemPayload>(Data.GetPayload()) : nullptr;
		}
		auto Gather(const DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, std::vector<FContentBrowserItem>& Items) -> std::shared_ptr<FOperations>
		{
			std::vector<std::shared_ptr<FOperations>> Contexts;
			std::vector<std::shared_ptr<const FContentBrowserHostContext>> Policies;
			if (Data.empty()) return {};
			for (const auto& Entry : Data)
			{
				auto* Owner = Entry.GetOwnerDataSource();
				auto* Native = Owner ? Cast<DContentBrowserNativeDataSource>(Owner) : nullptr;
				auto P = Native && Native->GetOuter() == Source.GetOuter() ? Payload(*Native, Entry) : nullptr;
				if (!P) return {};
				if (!std::ranges::contains(Policies, P->Host))
				{
					Policies.push_back(P->Host);
					Contexts.push_back(std::make_shared<FOperations>(*P->Host));
				}
				Items.push_back(P->Item);
			}
			return FOperations::Combine(std::move(Contexts));
		}
		auto SaveMode(EContentBrowserItemSaveFlags Flags) -> std::optional<EAssetSaveMode>
		{
			const auto Bits = static_cast<uint8>(Flags);
			if (Bits > 3) return {};
			return (Bits & 1) ? EAssetSaveMode::LoadedDirtyPackage : EAssetSaveMode::CanonicalResave;
		}
		auto Destination(std::string_view Path) -> std::string
		{
			return ContentBrowser::Private::FContentBrowserPaths::VirtualToPhysical(std::string(Path));
		}
		auto Bind(DContentBrowserDataSource& Source, const FContentBrowserItem& Item, std::shared_ptr<const FContentBrowserHostContext> Host) -> FContentBrowserItemData
		{
			auto P = std::make_shared<FNativeItemPayload>();
			P->Item = Item;
			P->Item.InternalItems.clear();
			ResolveItemPath(P->Item);
			P->Host = std::move(Host);
			const auto Kind = P->Item.Kind;
			auto VirtualPath = P->Item.VirtualPath;
			return {Source, Kind, std::move(VirtualPath), std::move(P)};
		}
	} // namespace
	DContentBrowserNativeDataSource::DContentBrowserNativeDataSource(const FObjectInitializer& I)
		: Super(I)
	{
	}
	DContentBrowserAssetDataSource::DContentBrowserAssetDataSource(const FObjectInitializer& I)
		: Super(I)
	{
	}
	DContentBrowserFileDataSource::DContentBrowserFileDataSource(const FObjectInitializer& I)
		: Super(I)
	{
	}
	auto DContentBrowserAssetDataSource::GetDataSourceName() const -> FName { return FName("Asset"); }
	auto DContentBrowserFileDataSource::GetDataSourceName() const -> FName { return FName("File"); }
	auto DContentBrowserAssetDataSource::OwnsKind(EContentBrowserItemKind K) const -> bool { return K != EContentBrowserItemKind::File; }
	auto DContentBrowserFileDataSource::OwnsKind(EContentBrowserItemKind K) const -> bool { return K == EContentBrowserItemKind::File || K == EContentBrowserItemKind::Folder; }
	auto DContentBrowserNativeDataSource::CreateItemData(const FContentBrowserItem& Item, const FContentBrowserHostContext& Host) -> FContentBrowserItemData
	{
		if (!IsActive() || !OwnsKind(Item.Kind)) return {};
		return Bind(*this, Item, std::make_shared<const FContentBrowserHostContext>(ScopedHost(*this, Host)));
	}
	auto DContentBrowserNativeDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Filter, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserHostContext& Host) -> void
	{
		if (!IsActive()) return;
		const auto Generation = GetActivationGeneration();
		const auto MountRevision = FMountPaths::GetMountRegistryRevision();
		auto Policy = std::make_shared<const FContentBrowserHostContext>(ScopedHost(*this, Host));
		std::shared_ptr<const ContentBrowser::Private::FContentBrowserItemsSnapshot> Snapshot;
		if (auto Extra = std::dynamic_pointer_cast<const ContentBrowser::Private::FNativeContentBrowserFilter>(Filter.Filter.ExtraFilter)) Snapshot = Extra->Snapshot;
		if (!Snapshot)
		{
			ContentBrowser::Private::FContentBrowserDataQuery Query;
			auto Capture = Query.PrepareItemsCapture(Destination(Filter.Path), Filter.Filter.bRecursive, Query.PrepareDiscoveryCatalog(), {});
			if (!Capture) return;
			Snapshot = Capture();
		}
		for (auto Item : Snapshot->Items)
		{
			if (!IsActive() || Generation != GetActivationGeneration() || MountRevision != FMountPaths::GetMountRegistryRevision()) return;
			if (!OwnsKind(Item.Kind) || (Filter.Filter.Kind && Item.Kind != *Filter.Filter.Kind)) continue;
			ResolveItemPath(Item);
			Item.InternalItems = {Bind(*this, Item, Policy)};
			if (!Callback(std::move(Item))) return;
		}
	}
	auto DContentBrowserNativeDataSource::CanHandleNativeBatch(std::span<const FContentBrowserItemData> Data) const -> bool
	{
		if (!IsActive() || Data.empty()) return false;
		return std::ranges::all_of(Data, [&](const auto& Item) {
			auto* Source = Item.GetOwnerDataSource();
			return Source && Cast<DContentBrowserNativeDataSource>(Source) && Source->GetOuter() == GetOuter();
		});
	}
	auto DContentBrowserNativeDataSource::CanEditItem(const FContentBrowserItemData& Data, std::string*) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && P->Item.Kind != EContentBrowserItemKind::Folder && (P->Item.Kind == EContentBrowserItemKind::File || bool(P->Host->OpenAsset));
	}
	auto DContentBrowserNativeDataSource::EditItem(const FContentBrowserItemData& Data) -> bool
	{
		auto P = Payload(*this, Data);
		if (!P || !CanEditItem(Data)) return false;
		const auto& Item = P->Item;
		if (Item.Kind == EContentBrowserItemKind::Redirector)
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Item.VirtualPath, Path)) return false;
			const auto Result = ResolveAssetObjectPathForOperation(Path);
			return Result && Result.FinalAssetData && P->Host->OpenAsset(Result.FinalPath.ToString(), Result.FinalAssetData->AssetClassName);
		}
		if (Item.Kind == EContentBrowserItemKind::Asset) return P->Host->OpenAsset(Item.VirtualPath, Item.AssetClassName);
#ifdef _WIN32
		const auto Path = std::filesystem::path(Item.PhysicalPath).make_preferred().wstring();
		return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", Path.c_str(), nullptr, nullptr, SW_SHOW)) > 32;
#else
		return false;
#endif
	}
	auto DContentBrowserNativeDataSource::CanDuplicateItem(const FContentBrowserItemData& Data, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && FOperations(*P->Host).CanDuplicateItem(P->Item, Error);
	}
	auto DContentBrowserNativeDataSource::DuplicateItem(const FContentBrowserItemData& Data, FContentBrowserItem& Out) -> bool
	{
		auto P = Payload(*this, Data);
		if (!P) return false;
		const bool Result = FOperations(*P->Host).DuplicateItem(P->Item, Out);
		if (!Out.StableId().empty())
		{
			ResolveItemPath(Out);
			if (IsActive()) Out.InternalItems = {Bind(*this, Out, P->Host)};
		}
		return Result;
	}
	auto DContentBrowserNativeDataSource::CanCreateFolder(std::string_view Path, std::string* Error) const -> bool
	{
		if (!IsActive()) return false;
		const std::filesystem::path Virtual(Path);
		FOperations Context(ScopedHost(*const_cast<DContentBrowserNativeDataSource*>(this), {}));
		return Context.CanCreateFolder(Destination(Virtual.parent_path().generic_string()), Virtual.filename().string(), Error);
	}
	auto DContentBrowserNativeDataSource::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserHostContext& Host) -> bool
	{
		if (!IsActive()) return false;
		const std::filesystem::path Virtual(Path);
		const auto Directory = Destination(Virtual.parent_path().generic_string());
		auto Policy = std::make_shared<const FContentBrowserHostContext>(ScopedHost(*this, Host));
		auto Operations = std::make_shared<FOperations>(*Policy);
		if (!Operations->CanCreateFolder(Directory, Virtual.filename().string())) return false;
		FContentBrowserItem Item{.Kind = EContentBrowserItemKind::Folder, .Name = Virtual.filename().string(), .VirtualPath = std::string(Path), .PhysicalPath = Destination(Path)};
		auto Data = Bind(*this, Item, Policy);
		Item.InternalItems = {Data};
		Out = {std::move(Item), [Data, Operations, Directory](std::string_view Name, std::string* Error) { return Data.GetOwnerDataSource() && Operations->CanCreateFolder(Directory, Name, Error); }, [Data, Operations, Directory, Policy](std::string_view Name, FContentBrowserItem& Result) {
    auto* Owner=Data.GetOwnerDataSource();
    if (!Owner) return false;
    TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Owner);
    if (!Operations->CreateFolder(Directory,Name,Result)) return false;
    ResolveItemPath(Result);
    if (Data.GetOwnerDataSource()) Result.InternalItems={Bind(*Owner,Result,Policy)};
    return true; }};
		return true;
	}
	auto DContentBrowserNativeDataSource::FixUpRedirectors(std::span<const FPackagePath> Paths, const FContentBrowserHostContext& Host) -> bool
	{
		FOperations Operations(ScopedHost(*this, Host));
		return IsActive() && Operations.FixUpRedirectors(Paths);
	}
	auto DContentBrowserNativeDataSource::FixUpRedirectorsInFolder(std::string_view Path, const FContentBrowserHostContext& Host) -> bool
	{
		FOperations Operations(ScopedHost(*this, Host));
		return IsActive() && Operations.FixUpRedirectorsInFolder(Path);
	}
	auto DContentBrowserNativeDataSource::FixUpAllRedirectors(const FContentBrowserHostContext& Host) -> bool
	{
		FOperations Operations(ScopedHost(*this, Host));
		return IsActive() && Operations.FixUpAllRedirectors();
	}

	auto DContentBrowserNativeDataSource::CanSaveItem(const FContentBrowserItemData& Item, EContentBrowserItemSaveFlags Flags, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Item);
		auto Mode = SaveMode(Flags);
		if (Flags == EContentBrowserItemSaveFlags::SaveOnlyIfLoaded && (!P || !FindResidentPackage(P->Item.PackagePath))) return false;
		return P && Mode && FOperations(*P->Host).CanSaveItem(P->Item, *Mode, Error);
	}
	auto DContentBrowserNativeDataSource::BulkSaveItems(std::span<const FContentBrowserItemData> Data, EContentBrowserItemSaveFlags Flags) -> bool
	{
		std::vector<FContentBrowserItem> Items;
		auto Operations = Gather(*this, Data, Items);
		auto Mode = SaveMode(Flags);
		std::vector<FStrongObjectPtr> LoadedRoots;
		if (Flags == EContentBrowserItemSaveFlags::SaveOnlyIfLoaded)
			for (const auto& Item : Items)
			{
				auto* Package = FindResidentPackage(Item.PackagePath);
				if (!Package) return false;
				LoadedRoots.emplace_back(Package);
				for (auto* Export : Package->GetTopLevelAssets())
					LoadedRoots.emplace_back(Export);
			}
		return Operations && Mode && Operations->BulkSaveItems(Items, *Mode);
	}
	auto DContentBrowserNativeDataSource::CanRenameItem(const FContentBrowserItemData& Item, std::string_view Name, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Item);
		return P && FOperations(*P->Host).CanRenameItem(P->Item, Name, Error);
	}
	auto DContentBrowserNativeDataSource::RenameItem(const FContentBrowserItemData& Item, std::string_view Name, FContentBrowserItem& Out) -> bool
	{
		auto P = Payload(*this, Item);
		if (!P || !FOperations(*P->Host).RenameItem(P->Item, Name, Out)) return false;
		ResolveItemPath(Out);
		if (IsActive())
		{
			Out.InternalItems = {Bind(*this, Out, P->Host)};
		}
		return true;
	}
	auto DContentBrowserNativeDataSource::CanCopyItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Item);
		return P && !Destination(Dest).empty() && FOperations(*P->Host).CanCopyItem(P->Item, Destination(Dest), Error);
	}
	auto DContentBrowserNativeDataSource::CanMoveItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Item);
		return P && !Destination(Dest).empty() && FOperations(*P->Host).CanMoveItem(P->Item, Destination(Dest), Error);
	}
	auto DContentBrowserNativeDataSource::BulkCopyItems(std::span<const FContentBrowserItemData> Data, std::string_view Dest) -> bool
	{
		std::vector<FContentBrowserItem> Items;
		auto Operations = Gather(*this, Data, Items);
		if (!Operations || Destination(Dest).empty()) return false;
		const bool Result = Operations->BulkCopyItems(Items, Destination(Dest));
		return Result;
	}
	auto DContentBrowserNativeDataSource::BulkMoveItems(std::span<const FContentBrowserItemData> Data, std::string_view Dest) -> bool
	{
		std::vector<FContentBrowserItem> Items;
		auto Operations = Gather(*this, Data, Items);
		if (!Operations || Destination(Dest).empty()) return false;
		const bool Result = Operations->BulkMoveItems(Items, Destination(Dest));
		return Result;
	}
	auto DContentBrowserNativeDataSource::CanDeleteItem(const FContentBrowserItemData& Item, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Item);
		if (!P) return false;
		if (!FOperations(*P->Host).CanModify(Error)) return false;
		const auto Mount = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(P->Item.PhysicalPath);
		const bool Allowed = Mount && Mount.Mount->bContentWritable
							 && !P->Item.PhysicalPath.empty()
							 && std::filesystem::path(P->Item.PhysicalPath).lexically_normal() != Mount.Mount->GetContentDir().lexically_normal();
		if (!Allowed && Error) *Error = "Deletion requires an item inside a writable content mount.";
		return Allowed;
	}
	auto DContentBrowserNativeDataSource::PrepareDeleteItems(std::span<const FContentBrowserItemData> Data) -> FContentBrowserDeleteContext
	{
		std::vector<FContentBrowserItem> Items;
		auto Operations = Gather(*this, Data, Items);
		if (!Operations) return {};
		auto Request = std::make_unique<FNativeDeleteRequest>();
		Request->Operations = Operations;
		Request->Plan = Operations->PrepareDeleteItems(Items);
		for (const auto& Entry : Data)
			Request->Payloads.push_back(Entry.GetPayload());
		return {Data.front(), std::move(Request)};
	}
	auto DContentBrowserNativeDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData> Data, FContentBrowserDeleteContext& Context) -> bool
	{
		if (Context.OwnerItem.GetOwnerDataSource() != this) return false;
		auto* Request = dynamic_cast<FNativeDeleteRequest*>(Context.Request.get());
		if (!Request || Data.size() != Request->Payloads.size()) return false;
		for (size_t I = 0; I < Data.size(); ++I)
			if (!CanHandleNativeBatch(Data) || Data[I].GetPayload() != Request->Payloads[I]) return false;
		const bool Result = Request->Operations->DeleteItems(Request->Plan);
		if (Result || !Request->Plan) Context.Request.reset();
		return Result;
	}
} // namespace Durin
