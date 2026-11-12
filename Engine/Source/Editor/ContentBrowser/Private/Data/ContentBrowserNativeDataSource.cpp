#include "ContentBrowser/ContentBrowserContracts.h"
#include "Data/ContentBrowserNativeUpdates.h"
#include "ContentBrowserNativeDataSource.h"
#include "ContentBrowserDataSubsystem.h"
#include "Data/ContentBrowserNativeItemPayloads.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "Data/ContentBrowserAssetActions.h"
#include "Data/ContentBrowserFolderOperation.h"
#include "Data/ContentBrowserAssetSave.h"
#include "Data/ContentBrowserNativeTransfer.h"
#include "Operations/ContentBrowserTransferPaths.h"
#include "Data/ContentBrowserDeletionOperation.h"
#include "Data/ContentBrowserDeleteRequestLease.h"
#include "AssetRegistry/Catalog.h"

#include "Asset/RegistryOperations.h"
#ifdef _WIN32
	#include <shellapi.h>
#endif
#include "Operations/ContentBrowserPaths.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "Editor/EditorEngine.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Package.h"
#include "Asset/Load.h"
namespace Durin
{
	using namespace ContentBrowser;
	namespace
	{
		using ContentBrowser::Private::FContentBrowserNativeItem;
		using FAssetActions = ContentBrowser::Private::FContentBrowserAssetActions;
		using FFolderOperation = ContentBrowser::Private::FContentBrowserFolderOperation;
		auto ResolveItemPath(FContentBrowserNativeItem& Item) -> void
		{
			if (Item.Kind != EContentBrowserItemKind::File && Item.Kind != EContentBrowserItemKind::Folder) return;
			const auto Path = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(Item.PhysicalPath);
			if (Path) Item.VirtualPath = Path.VirtualPath;
		}
		auto QueueCommittedUpdates(DContentBrowserDataSource& Source, const FContentChangeBatch& Batch, bool FromDiscovery) -> void
		{
			auto MakeData = [&](const FContentChange& Change, bool Old, DContentBrowserDataSource* Requested = nullptr) {
				auto& Producer = Requested ? *Requested : Source;
				FContentBrowserNativeItem Item;
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
				auto Data = Producer.CreateItemData(ContentBrowser::Private::MakeNativeItem(Item, Cast<DContentBrowserAssetDataSource>(&Producer) != nullptr));
				if (!Data.GetOwnerDataSource())
					if (auto* Service = Cast<DContentBrowserDataSubsystem>(Producer.GetOuter()))
						if (auto* Peer = Service->GetActiveDataSource(FName(Item.Kind == EContentBrowserItemKind::File ? "File" : "Asset"))) Data = Peer->CreateItemData(ContentBrowser::Private::MakeNativeItem(Item, Cast<DContentBrowserAssetDataSource>(Peer) != nullptr));

				return Data;
			};
			auto Queue = [](FContentBrowserItemDataUpdate Update) {
				if (auto* Owner = Update.GetItemData().GetOwnerDataSource()) Owner->QueueItemDataUpdate(std::move(Update));
			};

			auto QueueChange = [&](const FContentChange& Change, DContentBrowserDataSource* Requested = nullptr) {
				if (Change.Kind == EContentChangeKind::Removed)
					Queue(FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(MakeData(Change, true, Requested)));
				else if (Change.Kind == EContentChangeKind::Added)
					Queue(FContentBrowserItemDataUpdate::MakeItemAddedUpdate(MakeData(Change, false, Requested)));
				else if (Change.Kind == EContentChangeKind::Renamed)
				{
					auto Previous = MakeData(Change, true, Requested).GetVirtualPath();
					Queue(FContentBrowserItemDataUpdate::MakeItemMovedUpdate(MakeData(Change, false, Requested), std::move(Previous)));
				}
				else
					Queue(FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(MakeData(Change, false, Requested)));
			};
			for (const auto& Change : Batch.Changes)
			{
				if (!FromDiscovery && Cast<DContentBrowserAssetDataSource>(&Source) && !Change.bDirectory && (!Change.OldAssetPath.empty() || !Change.NewAssetPath.empty())) continue;
				QueueChange(Change);
				if (Change.bDirectory)
					if (auto* Service = Cast<DContentBrowserDataSubsystem>(Source.GetOuter()))
						if (auto* Peer = (Service->GetActiveDataSource(FName(Cast<DContentBrowserAssetDataSource>(&Source) ? "File" : "Asset"))))
							QueueChange(Change, Peer);
			}
			if (Batch.bFullRefresh)
				for (const auto& Mount : ContentBrowser::Private::FContentBrowserPaths::CaptureMounts())
				{
					FContentBrowserNativeItem Folder{.Kind = EContentBrowserItemKind::Folder, .VirtualPath = Mount.VirtualRoot, .PhysicalPath = Mount.PhysicalRoot};
					Queue(FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(Source.CreateItemData(ContentBrowser::Private::MakeNativeItem(Folder, Cast<DContentBrowserAssetDataSource>(&Source) != nullptr))));
				}
		}
		auto ScopedHost(DContentBrowserDataSource& Source, FContentBrowserOperationContext Host) -> FContentBrowserOperationContext
		{
			const TWeakObjectPtr<DContentBrowserDataSource> Weak(&Source);
			const auto Generation = Source.GetActivationGeneration();
			const auto MountRevision = FMountPaths::GetMountRegistryRevision();
			Host.NotifyScopedContentMutation = [Weak, Generation, MountRevision, Notify = Host.NotifyScopedContentMutation, Fallback = Host.NotifyMountedContentMutation](FContentChangeBatch Batch) {
				if (auto* Owner = Weak.Get(); Owner && Owner->IsActive() && Owner->GetActivationGeneration() == Generation
											  && FMountPaths::GetMountRegistryRevision() == MountRevision) ContentBrowser::Private::QueueNativeCommittedUpdates(*Owner, Batch);
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
		// Transient bridge for the legacy executor; never stored in an item payload.
		struct FNativePayloadView
		{
			FContentBrowserNativeItem Item;
			// Immutable request policy belongs to the item; execution state belongs to a command.
			std::shared_ptr<const FContentBrowserOperationContext> Host;
		};
		struct FNativeDeleteRequest final : IContentBrowserDeleteRequest
		{
			std::shared_ptr<ContentBrowser::Private::FContentBrowserDeletionOperation> Operations;
			std::unordered_map<std::string, std::vector<FContentBrowserItemContributionKey>> EffectKeys;
			std::function<void()> RebuildKeys;
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
			auto ValidateForExecution() -> EContentBrowserDeleteValidation override
			{
				const auto Status = Operations->ValidateForExecution();
				return Status ? EContentBrowserDeleteValidation::Valid : Status.Error == EAssetWriteError::StaleData ? EContentBrowserDeleteValidation::Stale : EContentBrowserDeleteValidation::Rejected;
			}
		};
		auto Payload(const DContentBrowserDataSource& Source, const FContentBrowserItemData& Data) -> std::optional<FNativePayloadView>
		{
			using namespace ContentBrowser::Private;
			if (Data.GetOwnerDataSource()!=&Source) return {};
			const auto MakeView=[&Data](const auto& Payload, EContentBrowserItemKind Kind) {
				return FNativePayloadView{.Item={.Kind=Kind, .Name=Data.GetItemName(), .VirtualPath=Data.GetVirtualPath(), .PhysicalPath=Payload.Location.PhysicalPath}, .Host=Payload.Host};
			};
			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserAssetFileItemDataPayload>(Data.GetPayload()))
			{
				auto View=MakeView(*P, P->bRedirector ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset);
				View.Item.PackagePath=P->PackagePath; View.Item.AssetClassName=P->AssetClassName; View.Item.Extension=P->Extension;
				View.Item.RedirectDestination=P->RedirectDestination; View.Item.FileSize=P->FileSize;
				View.Item.PackageFileSize=P->PackageFileSize; View.Item.BulkFileSize=P->BulkFileSize; View.Item.LastWriteTime=P->LastWriteTime;
				return View;
			}
			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserFileItemDataPayload>(Data.GetPayload()))
			{
				auto View=MakeView(*P, EContentBrowserItemKind::File);
				View.Item.Extension=P->Extension; View.Item.FileSize=P->FileSize; View.Item.LastWriteTime=P->LastWriteTime;
				return View;
			}
			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserAssetFolderItemDataPayload>(Data.GetPayload())) return MakeView(*P, EContentBrowserItemKind::Folder);
			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserFolderItemDataPayload>(Data.GetPayload())) return MakeView(*P, EContentBrowserItemKind::Folder);
			return {};
		}

		auto GatherDeletion(const DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, std::vector<FContentBrowserNativeItem>& Items) -> std::shared_ptr<ContentBrowser::Private::FContentBrowserDeletionOperation>
		{
			std::vector<std::shared_ptr<const FContentBrowserOperationContext>> Policies;
			bool Assets = false, Files = false;
			for (const auto& Entry : Data)
			{
				auto* Owner = Entry.GetOwnerDataSource();
				const auto P = Owner == &Source ? Payload(*Owner, Entry) : std::optional<FNativePayloadView>{};
				if (!P) return {};
				Assets |= Cast<DContentBrowserAssetDataSource>(Owner) != nullptr;
				Files |= Cast<DContentBrowserFileDataSource>(Owner) != nullptr;
				Policies.push_back(P->Host);
				if (std::ranges::none_of(Items, [&](const auto& Item) { return std::filesystem::path(Item.PhysicalPath).lexically_normal() == std::filesystem::path(P->Item.PhysicalPath).lexically_normal(); })) Items.push_back(P->Item);
			}
			if (Policies.empty()) return {};
			auto Host = *Policies.front();
			Host.CanMutateContent = [Policies] { for (const auto& Policy : Policies) if (Policy->CanMutateContent && !Policy->CanMutateContent()) return false; return true; };
			return std::make_shared<ContentBrowser::Private::FContentBrowserDeletionOperation>(Host, Assets, Files);
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
		auto Bind(DContentBrowserDataSource& Source, const FContentBrowserItem& Item, std::shared_ptr<const FContentBrowserOperationContext> Host) -> FContentBrowserItemData
		{
			using namespace ContentBrowser::Private;
			const auto* Description = Item.GetPrimaryInternalItem();
			if (!Description) return {};
			std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
			auto Kind = EContentBrowserItemKind::Folder;
			std::string Physical;
			auto Clone = [&]<typename T>(std::shared_ptr<const T> Existing) {
				if (!Existing) return false;
				Physical = Existing->Location.PhysicalPath;
				auto Copy = std::make_shared<T>(*Existing); Copy->Host = Host; Payload = std::move(Copy); return true;
			};
			if (Cast<DContentBrowserAssetDataSource>(&Source))
			{
				if (const auto File = std::dynamic_pointer_cast<const FContentBrowserAssetFileItemDataPayload>(Description->GetPayload()))
				{
					Kind = File->bRedirector ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset; Clone(File);
				}
				else Clone(std::dynamic_pointer_cast<const FContentBrowserAssetFolderItemDataPayload>(Description->GetPayload()));
			}
			else
			{
				if (const auto File = std::dynamic_pointer_cast<const FContentBrowserFileItemDataPayload>(Description->GetPayload()))
				{
					Kind = EContentBrowserItemKind::File; Clone(File);
				}
				else Clone(std::dynamic_pointer_cast<const FContentBrowserFolderItemDataPayload>(Description->GetPayload()));
			}
			if (!Payload) return {};
			auto Virtual = Description->GetVirtualPath();
			if (Virtual.empty()) { const auto Mount = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(Physical); if (Mount) Virtual = Mount.VirtualPath; }
			return {Source, Description->GetItemFlags(), std::move(Virtual), Description->GetItemName(), Description->GetDisplayName(), std::move(Payload)};
		}
		auto BindNative(DContentBrowserDataSource& Source, const FContentBrowserNativeItem& Native, std::shared_ptr<const FContentBrowserOperationContext> Host) -> FContentBrowserItemData
		{
			auto Resolved = Native; ResolveItemPath(Resolved);
			return Bind(Source, ContentBrowser::Private::MakeNativeItem(Resolved, Cast<DContentBrowserAssetDataSource>(&Source) != nullptr), std::move(Host));
		}
	} // namespace
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
	static auto NativeCreateItemData(DContentBrowserDataSource& Source, const FContentBrowserItem& Item, const FContentBrowserOperationContext& Host) -> FContentBrowserItemData
	{
		if (!Source.IsActive()) return {};
		return Bind(Source, Item, std::make_shared<const FContentBrowserOperationContext>(ScopedHost(Source, Host)));
	}
	auto DContentBrowserAssetDataSource::CreateItemData(const FContentBrowserItem& Item, const FContentBrowserOperationContext& Host) -> FContentBrowserItemData
	{ return NativeCreateItemData(*this, Item, Host); }
	auto DContentBrowserFileDataSource::CreateItemData(const FContentBrowserItem& Item, const FContentBrowserOperationContext& Host) -> FContentBrowserItemData
	{ return NativeCreateItemData(*this, Item, Host); }

	static auto NativeEnumerateItemsMatchingFilter(DContentBrowserDataSource& Source, const FContentBrowserDataCompiledFilter& Filter, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserOperationContext& Host) -> void
	{
		if (!Source.IsActive()) return;
		const auto Generation = Source.GetActivationGeneration();
		const auto MountRevision = FMountPaths::GetMountRegistryRevision();
		auto Policy = std::make_shared<const FContentBrowserOperationContext>(ScopedHost(Source, Host));
		const auto Found = Filter.CompiledFilters.find(&Source);
		if (Found == Filter.CompiledFilters.end()) return;
		if (Found->second.Generation != Generation || Found->second.MountRevision != MountRevision
			|| Found->second.DiscoveryRevision != Source.GetDiscoveryRevision()) return;
		if (!Found->second.Data) return;
		const auto Snapshot = Found->second.Data->Capture({}, {});
		for (auto Item : Snapshot.Items)
		{
			if (!Source.IsActive() || Generation != Source.GetActivationGeneration() || MountRevision != FMountPaths::GetMountRegistryRevision()) return;
			auto Bound = Bind(Source, Item, Policy);
			if (!Bound.GetOwnerDataSource()) continue;
			if (Filter.Filter.Kind) { const auto Native = Payload(Source, Bound); if (!Native || Native->Item.Kind != *Filter.Filter.Kind) continue; }
			Item.InternalItems = {std::move(Bound)};
			if (!Callback(std::move(Item))) return;
		}
	}
	auto DContentBrowserAssetDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Filter, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserOperationContext& Host) -> void
	{ return NativeEnumerateItemsMatchingFilter(*this, Filter, Callback, Host); }
	auto DContentBrowserFileDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Filter, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserOperationContext& Host) -> void
	{ return NativeEnumerateItemsMatchingFilter(*this, Filter, Callback, Host); }

	namespace
	{
		auto FinishAttribute(std::string_view Key, bool IncludeMetaData, FContentBrowserItemDataAttributeValue& Out) -> bool
		{
			if (!Out.IsValid()) return false;
			if (IncludeMetaData) Out.SetMetaData({.DisplayName=std::string(Key), .Suffix=Key==ContentBrowserItemAttributes::ItemDiskSize || Key==ContentBrowserItemAttributes::PackageFileSize || Key==ContentBrowserItemAttributes::BulkFileSize ? "bytes" : ""});
			return true;
		}
		auto CollectAttributes(const DContentBrowserDataSource& Source, const FContentBrowserItemData& Data, bool IncludeMetaData, std::initializer_list<std::string_view> Keys, FContentBrowserItemDataAttributeValues& Out) -> bool
		{
			Out.clear();
			for (const auto Key : Keys)
			{
				FContentBrowserItemDataAttributeValue Value;
				if (Source.GetItemAttribute(Data, IncludeMetaData, Key, Value)) Out.emplace(std::string(Key), std::move(Value));
			}
			return !Out.empty();
		}
		auto AssetTypeDisplayName(const ContentBrowser::Private::FContentBrowserAssetFileItemDataPayload& File) -> std::string
		{
			if (File.bRedirector) return "Redirector";
			if (const auto Type=FindAssetTypePresentation(File.AssetClassName); Type && !Type->DisplayName.empty()) return Type->DisplayName;
			const auto Separator=File.AssetClassName.rfind("::");
			auto Name=Separator==std::string::npos ? File.AssetClassName : File.AssetClassName.substr(Separator+2);
			if (Name.starts_with('D') && Name.size()>1) Name.erase(Name.begin());
			return Name;
		}
	}
	auto DContentBrowserAssetDataSource::GetItemAttribute(const FContentBrowserItemData& Data, bool IncludeMetaData, std::string_view Key, FContentBrowserItemDataAttributeValue& Out) const -> bool
	{
		using namespace ContentBrowser::Private;
		using namespace ContentBrowserItemAttributes;
		Out={};
		if (Data.GetOwnerDataSource()!=this) return false;
		const auto File=std::dynamic_pointer_cast<const FContentBrowserAssetFileItemDataPayload>(Data.GetPayload());
		const auto Folder=std::dynamic_pointer_cast<const FContentBrowserAssetFolderItemDataPayload>(Data.GetPayload());
		if (!File && !Folder) return false;
		if (Key==ItemTypeName) Out=FContentBrowserItemDataAttributeValue(File ? File->AssetClassName : std::string("Folder"));
		else if (Key==ItemTypeDisplayName) Out=FContentBrowserItemDataAttributeValue(File ? AssetTypeDisplayName(*File) : std::string("Folder"));
		else if (File)
		{
			if (Key==ItemDiskSize) Out=FContentBrowserItemDataAttributeValue(static_cast<uint64>(File->FileSize));
			else if (Key==PackageFileSize) Out=FContentBrowserItemDataAttributeValue(static_cast<uint64>(File->PackageFileSize));
			else if (Key==BulkFileSize) Out=FContentBrowserItemDataAttributeValue(static_cast<uint64>(File->BulkFileSize));
			else if (Key==LastWriteTime) Out=FContentBrowserItemDataAttributeValue(static_cast<int64>(File->LastWriteTime.time_since_epoch().count()));
			else if (Key==Class) Out=FContentBrowserItemDataAttributeValue(File->AssetClassName);
			else if (Key==Extension) Out=FContentBrowserItemDataAttributeValue(File->Extension);
			else if (Key==AssetPath && File->AssetPath.IsValid()) Out=FContentBrowserItemDataAttributeValue(File->AssetPath.ToString());
			else if (Key==PackagePath && !File->PackagePath.GetView().empty()) Out=FContentBrowserItemDataAttributeValue(std::string(File->PackagePath.GetView()));
			else if (Key==IsRedirector) Out=FContentBrowserItemDataAttributeValue(File->bRedirector);
			else if (Key==RedirectDestination && File->bRedirector) Out=FContentBrowserItemDataAttributeValue(File->RedirectDestination.ToString());
		}
		return FinishAttribute(Key, IncludeMetaData, Out);
	}
	auto DContentBrowserFileDataSource::GetItemAttribute(const FContentBrowserItemData& Data, bool IncludeMetaData, std::string_view Key, FContentBrowserItemDataAttributeValue& Out) const -> bool
	{
		using namespace ContentBrowser::Private;
		using namespace ContentBrowserItemAttributes;
		Out={};
		if (Data.GetOwnerDataSource()!=this) return false;
		const auto File=std::dynamic_pointer_cast<const FContentBrowserFileItemDataPayload>(Data.GetPayload());
		const auto Folder=std::dynamic_pointer_cast<const FContentBrowserFolderItemDataPayload>(Data.GetPayload());
		if (!File && !Folder) return false;
		if (Key==ItemTypeName) Out=FContentBrowserItemDataAttributeValue(File ? File->Extension : std::string("Folder"));
		else if (Key==ItemTypeDisplayName) Out=FContentBrowserItemDataAttributeValue(Folder ? std::string("Folder") : File->Extension.empty() ? std::string("File") : File->Extension.substr(1)+" file");
		else if (File)
		{
			if (Key==ItemDiskSize) Out=FContentBrowserItemDataAttributeValue(static_cast<uint64>(File->FileSize));
			else if (Key==Extension) Out=FContentBrowserItemDataAttributeValue(File->Extension);
			else if (Key==LastWriteTime) Out=FContentBrowserItemDataAttributeValue(static_cast<int64>(File->LastWriteTime.time_since_epoch().count()));
		}
		return FinishAttribute(Key, IncludeMetaData, Out);
	}
	auto DContentBrowserAssetDataSource::GetItemAttributes(const FContentBrowserItemData& Data, bool IncludeMetaData, FContentBrowserItemDataAttributeValues& Out) const -> bool
	{
		using namespace ContentBrowserItemAttributes;
		return CollectAttributes(*this, Data, IncludeMetaData, {ItemTypeName, ItemTypeDisplayName, Class, ItemDiskSize, AssetPath, PackagePath, Extension, IsRedirector, RedirectDestination, PackageFileSize, BulkFileSize, LastWriteTime}, Out);
	}
	auto DContentBrowserAssetDataSource::GetItemPhysicalPath(const FContentBrowserItemData& Data, std::string& Out) const -> bool
	{
		using namespace ContentBrowser::Private;
		Out.clear();
		if (Data.GetOwnerDataSource()!=this) return false;
		if (const auto File=std::dynamic_pointer_cast<const FContentBrowserAssetFileItemDataPayload>(Data.GetPayload())) Out=File->Location.PhysicalPath;
		else if (const auto Folder=std::dynamic_pointer_cast<const FContentBrowserAssetFolderItemDataPayload>(Data.GetPayload())) Out=Folder->Location.PhysicalPath;
		return !Out.empty();
	}
	auto DContentBrowserFileDataSource::GetItemAttributes(const FContentBrowserItemData& Data, bool IncludeMetaData, FContentBrowserItemDataAttributeValues& Out) const -> bool
	{
		using namespace ContentBrowserItemAttributes;
		return CollectAttributes(*this, Data, IncludeMetaData, {ItemTypeName, ItemTypeDisplayName, ItemDiskSize, Extension, LastWriteTime}, Out);
	}
	auto DContentBrowserFileDataSource::GetItemPhysicalPath(const FContentBrowserItemData& Data, std::string& Out) const -> bool
	{
		using namespace ContentBrowser::Private;
		Out.clear();
		if (Data.GetOwnerDataSource()!=this) return false;
		if (const auto File=std::dynamic_pointer_cast<const FContentBrowserFileItemDataPayload>(Data.GetPayload())) Out=File->Location.PhysicalPath;
		else if (const auto Folder=std::dynamic_pointer_cast<const FContentBrowserFolderItemDataPayload>(Data.GetPayload())) Out=Folder->Location.PhysicalPath;
		return !Out.empty();
	}
	auto DContentBrowserAssetDataSource::CanEditItem(const FContentBrowserItemData& Data, std::string*) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && (P->Item.Kind == EContentBrowserItemKind::Asset || P->Item.Kind == EContentBrowserItemKind::Redirector) && bool(P->Host->OpenAsset);
	}
	auto DContentBrowserAssetDataSource::EditItem(const FContentBrowserItemData& Data) -> bool
	{
		auto P = Payload(*this, Data);
		if (!P || !CanEditItem(Data)) return false;
		if (P->Item.Kind == EContentBrowserItemKind::Redirector)
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(P->Item.VirtualPath, Path)) return false;
			const auto Result = ResolveAssetObjectPathForOperation(Path);
			return Result && Result.FinalAssetData && P->Host->OpenAsset(Result.FinalPath.ToString(), Result.FinalAssetData->AssetClassName);
		}
		return P->Host->OpenAsset(P->Item.VirtualPath, P->Item.AssetClassName);
	}
	auto DContentBrowserFileDataSource::CanEditItem(const FContentBrowserItemData& Data, std::string*) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && P->Item.Kind == EContentBrowserItemKind::File;
	}
	auto DContentBrowserFileDataSource::EditItem(const FContentBrowserItemData& Data) -> bool
	{
		auto P = Payload(*this, Data);
		if (!P || !CanEditItem(Data)) return false;
#ifdef _WIN32
		const auto Path = std::filesystem::path(P->Item.PhysicalPath).make_preferred().wstring();
		return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", Path.c_str(), nullptr, nullptr, SW_SHOW)) > 32;
#else
		return false;
#endif
	}
	auto DContentBrowserAssetDataSource::CanDuplicateItem(const FContentBrowserItemData& Data, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && FAssetActions(*P->Host).CanDuplicateItem(P->Item, Error);
	}
	auto DContentBrowserAssetDataSource::DuplicateItem(const FContentBrowserItemData& Data, FContentBrowserItemData& Out) -> bool
	{
		auto P = Payload(*this, Data);
		if (!P) return false;
		FContentBrowserNativeItem Native;
		const bool Result = FAssetActions(*P->Host).DuplicateItem(P->Item, Native);
		Out = {};
		if (!Native.PhysicalPath.empty() && IsActive()) Out = BindNative(*this, Native, P->Host);
		return Result;
	}
	static auto NativeCanCreateFolder(const DContentBrowserDataSource& Source, std::string_view Path, std::string* Error) -> bool
	{
		if (!Source.IsActive()) return false;
		const std::filesystem::path Virtual(Path);
		FFolderOperation Context(ScopedHost(*const_cast<DContentBrowserDataSource*>(&Source), {}));
		return Context.CanCreateFolder(Destination(Virtual.parent_path().generic_string()), Virtual.filename().string(), Error);
	}
	auto DContentBrowserAssetDataSource::CanCreateFolder(std::string_view Path, std::string* Error) const -> bool
	{ return NativeCanCreateFolder(*this, Path, Error); }
	auto DContentBrowserFileDataSource::CanCreateFolder(std::string_view Path, std::string* Error) const -> bool
	{ return NativeCanCreateFolder(*this, Path, Error); }

	static auto NativeCreateFolder(DContentBrowserDataSource& Source, std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserOperationContext& Host) -> bool
	{
		if (!Source.IsActive()) return false;
		const std::filesystem::path Virtual(Path);
		const auto Directory = Destination(Virtual.parent_path().generic_string());
		auto Policy = std::make_shared<const FContentBrowserOperationContext>(ScopedHost(Source, Host));
		auto Operations = std::make_shared<FFolderOperation>(*Policy);
		if (!Operations->CanCreateFolder(Directory, Virtual.filename().string())) return false;
		FContentBrowserNativeItem Native{.Kind = EContentBrowserItemKind::Folder, .Name = Virtual.filename().string(), .VirtualPath = std::string(Path), .PhysicalPath = Destination(Path)};
		auto Data = BindNative(Source, Native, Policy);
		Data = FContentBrowserItemData(Source, Data.GetItemFlags() | EContentBrowserItemFlags::Temporary_Creation, Data.GetVirtualPath(), Data.GetItemName(), Data.GetDisplayName(), Data.GetPayload());
		Out = {Data, [Data, Operations, Directory](std::string_view Name, std::string* Error) { return Data.GetOwnerDataSource() && Operations->CanCreateFolder(Directory, Name, Error); }, [Data, Operations, Directory, Policy](std::string_view Name, FContentBrowserItemData& Result) {
			auto* Owner = Data.GetOwnerDataSource();
			if (!Owner) return false;
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Owner);
			FContentBrowserNativeItem Created;
			if (!Operations->CreateFolder(Directory, Name, Created)) return false;
			Result = {};
			if (Data.GetOwnerDataSource()) Result = BindNative(*Owner, Created, Policy);
			return true;
		}};
		return true;
	}
	auto DContentBrowserAssetDataSource::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserOperationContext& Host) -> bool
	{ return NativeCreateFolder(*this, Path, Out, Host); }
	auto DContentBrowserFileDataSource::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserOperationContext& Host) -> bool
	{ return NativeCreateFolder(*this, Path, Out, Host); }

	auto DContentBrowserAssetDataSource::FixUpRedirectors(std::span<const FPackagePath> Paths, const FContentBrowserOperationContext& Host) -> bool
	{
		FAssetActions Operations(ScopedHost(*this, Host));
		return IsActive() && Operations.FixUpRedirectors(Paths);
	}
	auto DContentBrowserAssetDataSource::FixUpRedirectorsInFolder(std::string_view Path, const FContentBrowserOperationContext& Host) -> bool
	{
		FAssetActions Operations(ScopedHost(*this, Host));
		return IsActive() && Operations.FixUpRedirectorsInFolder(Path);
	}
	auto DContentBrowserAssetDataSource::FixUpAllRedirectors(const FContentBrowserOperationContext& Host) -> bool
	{
		FAssetActions Operations(ScopedHost(*this, Host));
		return IsActive() && Operations.FixUpAllRedirectors();
	}

	auto DContentBrowserAssetDataSource::CanSaveItem(const FContentBrowserItemData& Item, EContentBrowserItemSaveFlags Flags, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Item);
		auto Mode = SaveMode(Flags);
		if ((static_cast<uint8>(Flags) & static_cast<uint8>(EContentBrowserItemSaveFlags::SaveOnlyIfLoaded)) && (!P || !FindResidentPackage(P->Item.PackagePath))) return false;
		return P && Mode && ContentBrowser::Private::FContentBrowserAssetSaveOperation(*P->Host).CanSaveItem(P->Item, *Mode, Error);
	}
	auto DContentBrowserAssetDataSource::BulkSaveItems(std::span<const FContentBrowserItemData> Data, EContentBrowserItemSaveFlags Flags) -> FContentBrowserOperationResult
	{
		FContentBrowserOperationResult Result;
		const auto Mode = SaveMode(Flags);
		std::vector<FContentBrowserNativeItem> Items;
		std::vector<std::shared_ptr<const FContentBrowserOperationContext>> Policies;
		std::vector<FStrongObjectPtr> LoadedRoots;
		for (const auto& Entry : Data)
		{
			auto P = Payload(*this, Entry);
			if (!P || P->Item.Kind != EContentBrowserItemKind::Asset || !Mode)
			{
				for (const auto& Rejected : Data) Result.Add(Rejected.GetContributionKey(), EContentBrowserEffectStatus::Failed, "Save requires current asset contributions and supported save flags.");
				return Result;
			}
			Policies.push_back(P->Host);
			if (static_cast<uint8>(Flags) & static_cast<uint8>(EContentBrowserItemSaveFlags::SaveOnlyIfLoaded))
			{
				auto* Package = FindResidentPackage(P->Item.PackagePath);
				if (!Package)
				{
					Result.Add(Entry.GetContributionKey(), EContentBrowserEffectStatus::Failed, "SaveOnlyIfLoaded requires a resident package.");
					continue;
				}
				LoadedRoots.emplace_back(Package);
				for (auto* Export : Package->GetTopLevelAssets()) LoadedRoots.emplace_back(Export);
			}
			Items.push_back(P->Item);
		}
		if (Items.empty()) return Result;
		auto Host = *Policies.front();
		Host.CanMutateContent = [Policies] {
			for (const auto& Policy : Policies) if (Policy->CanMutateContent && !Policy->CanMutateContent()) return false;
			return true;
		};
		const auto Native = ContentBrowser::Private::FContentBrowserAssetSaveOperation(std::move(Host)).BulkSaveItems(Items, *Mode);
		Result.bStopped = Native.bStopped;
		Result.bCancelled = Native.bCancelled;
		for (const auto& Effect : Native.Effects)
			for (const auto& Entry : Data)
				if (const auto P = std::dynamic_pointer_cast<const ContentBrowser::Private::FContentBrowserAssetFileItemDataPayload>(Entry.GetPayload()); P && P->PackagePath == Effect.OriginalIdentity)
					Result.Add(Entry.GetContributionKey(), Effect.Status, Effect.Diagnostic);
		return Result;
	}
	namespace
	{
		auto PrepareNativeRename(DContentBrowserDataSource& Source, const FContentBrowserItemData& Item, std::string_view Name, bool Asset) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
		{
			auto P = Payload(Source, Item);
			if (!P) return {};
			ContentBrowser::Private::FNativeTransferInput Input{P->Item, Item.GetContributionKey(), P->Host};
			if (Name.empty() || Name == "." || Name == ".." || Name.find_first_of("/\\:*?\"<>|") != std::string_view::npos || Name.back() == '.' || Name.back() == ' ')
				return ContentBrowser::Private::TransferPaths::Rejected({Input}, "Enter a valid name without path separators.");
			auto Target = std::filesystem::path(P->Item.PhysicalPath).parent_path() / Name;
			if (P->Item.Kind == EContentBrowserItemKind::Asset) Target += ".dasset";
			else if (P->Item.Kind == EContentBrowserItemKind::File && Target.extension().empty()) Target += std::filesystem::path(P->Item.PhysicalPath).extension();
			std::unique_ptr<FContentBrowserItemDataTransferContext> Context;
			if (ContentBrowser::Private::TransferPaths::Same(Target, P->Item.PhysicalPath))
			{
				Context = std::make_unique<FContentBrowserItemDataTransferContext>(std::vector{Item.GetContributionKey()}, std::vector<FContentBrowserTransferDestination>{},
					[Item, Policy = P->Host](std::string*) { return Item.GetOwnerDataSource() && (!Policy->CanMutateContent || Policy->CanMutateContent()) && Item.GetOwnerDataSource(); },
					[](const auto&) { FContentBrowserOperationResult Result; Result.bNoChanges = true; return Result; });
				Context->SetResultItemGetter([Item] { return Item.GetOwnerDataSource() ? Item : FContentBrowserItemData{}; });
				return Context;
			}
			const auto Directory = std::filesystem::path(P->Item.PhysicalPath).parent_path().generic_string();
			Context = Asset ? ContentBrowser::Private::PrepareAssetTransfer({Input}, Directory, true, std::string(Name)) : ContentBrowser::Private::PrepareFileTransfer({Input}, Directory, true, std::string(Name));
			if (!Context) return {};
			auto Renamed = P->Item;
			Renamed.Name = std::string(Name); Renamed.PhysicalPath = Target.generic_string();
			Renamed.VirtualPath = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(Renamed.PhysicalPath).VirtualPath;
			if (Renamed.Kind == EContentBrowserItemKind::Asset)
			{
				auto Package = Renamed.VirtualPath; if (Package.ends_with(".dasset")) Package.resize(Package.size() - 7);
				FPackagePath::TryCreate(Package, Renamed.PackagePath);
				FTopLevelAssetPath Old, New;
				if (FTopLevelAssetPath::TryCreate(P->Item.VirtualPath, Old) && FTopLevelAssetPath::TryCreate(Renamed.PackagePath, Old.GetAssetName(), New)) Renamed.VirtualPath = New.ToString();
			}
			Context->SetResultItemGetter([Item, Renamed, Policy = P->Host] { auto* Owner = Item.GetOwnerDataSource(); return Owner ? BindNative(*Owner, Renamed, Policy) : FContentBrowserItemData{}; });
			return Context;
		}
	}
	namespace
	{
		auto PrepareNativeTransfer(DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, std::string_view Dest, bool Asset, bool Move) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
		{
			std::vector<ContentBrowser::Private::FNativeTransferInput> Inputs;
			for (const auto& Item : Data)
			{
				auto P = Payload(Source, Item);
				if (!P) return {};
				Inputs.push_back({P->Item, Item.GetContributionKey(), P->Host});
			}
			if (Inputs.empty()) return {};
			return Asset ? ContentBrowser::Private::PrepareAssetTransfer(std::move(Inputs), Destination(Dest), Move)
				: ContentBrowser::Private::PrepareFileTransfer(std::move(Inputs), Destination(Dest), Move);
		}
	}
	auto DContentBrowserAssetDataSource::PrepareRenameItem(const FContentBrowserItemData& Item, std::string_view Name) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		return PrepareNativeRename(*this, Item, Name, true);
	}
	auto DContentBrowserAssetDataSource::CanRenameItem(const FContentBrowserItemData& Item, std::string_view Name, std::string* Error) const -> bool
	{
		auto Context = PrepareNativeRename(*const_cast<DContentBrowserAssetDataSource*>(this), Item, Name, true);
		return Context && Context->Validate(Error);
	}
	auto DContentBrowserAssetDataSource::PrepareCopyItems(std::span<const FContentBrowserItemData> Data, std::string_view Dest) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		return PrepareNativeTransfer(*this, Data, Dest, true, false);
	}
	auto DContentBrowserAssetDataSource::CanCopyItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		auto Context = PrepareNativeTransfer(*const_cast<DContentBrowserAssetDataSource*>(this), std::span{&Item, 1}, Dest, true, false);
		return Context && Context->Validate(Error);
	}
	auto DContentBrowserAssetDataSource::PrepareMoveItems(std::span<const FContentBrowserItemData> Data, std::string_view Dest) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		return PrepareNativeTransfer(*this, Data, Dest, true, true);
	}
	auto DContentBrowserAssetDataSource::CanMoveItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		auto Context = PrepareNativeTransfer(*const_cast<DContentBrowserAssetDataSource*>(this), std::span{&Item, 1}, Dest, true, true);
		return Context && Context->Validate(Error);
	}
	auto DContentBrowserFileDataSource::PrepareRenameItem(const FContentBrowserItemData& Item, std::string_view Name) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		return PrepareNativeRename(*this, Item, Name, false);
	}
	auto DContentBrowserFileDataSource::CanRenameItem(const FContentBrowserItemData& Item, std::string_view Name, std::string* Error) const -> bool
	{
		auto Context = PrepareNativeRename(*const_cast<DContentBrowserFileDataSource*>(this), Item, Name, false);
		return Context && Context->Validate(Error);
	}
	auto DContentBrowserFileDataSource::PrepareCopyItems(std::span<const FContentBrowserItemData> Data, std::string_view Dest) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		return PrepareNativeTransfer(*this, Data, Dest, false, false);
	}
	auto DContentBrowserFileDataSource::CanCopyItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		auto Context = PrepareNativeTransfer(*const_cast<DContentBrowserFileDataSource*>(this), std::span{&Item, 1}, Dest, false, false);
		return Context && Context->Validate(Error);
	}
	auto DContentBrowserFileDataSource::PrepareMoveItems(std::span<const FContentBrowserItemData> Data, std::string_view Dest) -> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		return PrepareNativeTransfer(*this, Data, Dest, false, true);
	}
	auto DContentBrowserFileDataSource::CanMoveItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		auto Context = PrepareNativeTransfer(*const_cast<DContentBrowserFileDataSource*>(this), std::span{&Item, 1}, Dest, false, true);
		return Context && Context->Validate(Error);
	}
	static auto NativeCanDeleteItem(const DContentBrowserDataSource& Source, const FContentBrowserItemData& Item, std::string* Error) -> bool
	{
		auto P = Payload(Source, Item);
		if (!P) return false;
		if (!ContentBrowser::Private::FContentBrowserMutationPolicy(*P->Host).CanModify(Error)) return false;
		const auto Mount = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(P->Item.PhysicalPath);
		const bool Allowed = Mount && Mount.Mount->bContentWritable
							 && !P->Item.PhysicalPath.empty()
							 && std::filesystem::path(P->Item.PhysicalPath).lexically_normal() != Mount.Mount->GetContentDir().lexically_normal();
		if (!Allowed && Error) *Error = "Deletion requires an item inside a writable content mount.";
		return Allowed;
	}
	auto DContentBrowserAssetDataSource::CanDeleteItem(const FContentBrowserItemData& Item, std::string* Error) const -> bool
	{ return NativeCanDeleteItem(*this, Item, Error); }
	auto DContentBrowserFileDataSource::CanDeleteItem(const FContentBrowserItemData& Item, std::string* Error) const -> bool
	{ return NativeCanDeleteItem(*this, Item, Error); }

	static auto NativePrepareDeleteItems(DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data) -> FContentBrowserDeleteContext
	{
		std::vector<FContentBrowserNativeItem> Items;
		auto Operations = GatherDeletion(Source, Data, Items);
		if (!Operations) return {};
		auto Request = std::make_unique<FNativeDeleteRequest>();
		Request->Operations = Operations;
		Request->Plan = Operations->PrepareDeleteItems(Items);
		Request->RebuildKeys = [Request = Request.get(), Captured = std::vector<FContentBrowserItemData>(Data.begin(), Data.end())] {
		Request->EffectKeys.clear();
		const auto Catalog = CaptureAssetCatalogSnapshot();
		for (const auto& Effect : Request->Plan->Entries)
		{
			const bool AssetEffect = Effect.Kind == ContentBrowser::Private::EContentDeletionEntryKind::AssetPackage || Effect.Kind == ContentBrowser::Private::EContentDeletionEntryKind::ManagedCompanion;
			const bool Directory = Effect.Kind == ContentBrowser::Private::EContentDeletionEntryKind::Directory;
			for (const auto& Entry : Captured)
			{
				auto* Owner = Entry.GetOwnerDataSource();
				const auto P = Owner ? Payload(*Owner, Entry) : std::optional<FNativePayloadView>{};
				if (!P || (!Directory && AssetEffect != (Cast<DContentBrowserAssetDataSource>(Owner) != nullptr))) continue;
				std::filesystem::path Relative;
				if (!FPaths::TryMakeLexicalRelativePath(Effect.PhysicalPath, P->Item.PhysicalPath, Relative)) continue;
				auto Key = Entry.GetContributionKey();
				std::string Virtual = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(Effect.PhysicalPath).VirtualPath;
				if (AssetEffect)
					for (const auto& [Path, Package] : Catalog.Assets)
						if (std::filesystem::path(Package.PhysicalPath).lexically_normal() == std::filesystem::path(Effect.PhysicalPath).lexically_normal() && !Package.TopLevelAssets.empty()) Virtual = Package.TopLevelAssets.front().AssetPath.ToString();
				Key.Item = FContentBrowserItemDataKey(Directory ? EContentBrowserItemFlags::Type_Folder : EContentBrowserItemFlags::Type_File, Virtual);
				Request->EffectKeys[Effect.PhysicalPath].push_back(std::move(Key));
			}
		}

		};
		Request->RebuildKeys();
		for (const auto& Entry : Data)
			Request->Payloads.push_back(Entry.GetPayload());
		return {Data.front(), std::move(Request)};
	}
	auto DContentBrowserAssetDataSource::PrepareDeleteItems(std::span<const FContentBrowserItemData> Data) -> FContentBrowserDeleteContext
	{ return NativePrepareDeleteItems(*this, Data); }
	auto DContentBrowserFileDataSource::PrepareDeleteItems(std::span<const FContentBrowserItemData> Data) -> FContentBrowserDeleteContext
	{ return NativePrepareDeleteItems(*this, Data); }

	static auto NativeBulkDeleteItems(DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, FContentBrowserDeleteContext& Context) -> FContentBrowserOperationResult
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(&Source);
		const std::vector<FContentBrowserItemData> Snapshot(Data.begin(), Data.end());
		Data = Snapshot;
		FContentBrowserOperationResult Result;
		if (Context.OwnerItem.GetOwnerDataSource() != &Source) return Result;
		ContentBrowser::Private::FContentBrowserDeleteRequestLease Lease(Context);
		auto* Request = dynamic_cast<FNativeDeleteRequest*>(Lease.Request.get());
		if (!Request || Data.size() != Request->Payloads.size()) return Result;
		for (size_t I = 0; I < Data.size(); ++I)
			if (Data[I].GetOwnerDataSource() != &Source || Data[I].GetPayload() != Request->Payloads[I]) return Result;
		Request->RebuildKeys();
		const auto Native = Request->Operations->DeleteItems(Request->Plan);
		Result.bStopped = Native.bStopped; Result.bCancelled = Native.bCancelled; Result.bNoChanges = Native.bNoChanges;
		for (const auto& Effect : Native.Effects)
		{
			const auto Found = Request->EffectKeys.find(Effect.OriginalIdentity);
			if (Found != Request->EffectKeys.end())
				for (const auto& Key : Found->second) Result.Effects.push_back({Key, std::nullopt, Effect.Status, Effect.Diagnostic, Effect.RecoveryLocations});
			else if (!Data.empty()) Result.Effects.push_back({Data.front().GetContributionKey(), std::nullopt, Effect.Status, Effect.Diagnostic, Effect.RecoveryLocations});
		}
		Lease.bConsumed = !Request->Plan;
		return Result;
	}
	auto DContentBrowserAssetDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData> Data, FContentBrowserDeleteContext& Context) -> FContentBrowserOperationResult
	{ return NativeBulkDeleteItems(*this, Data, Context); }
	auto DContentBrowserFileDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData> Data, FContentBrowserDeleteContext& Context) -> FContentBrowserOperationResult
	{ return NativeBulkDeleteItems(*this, Data, Context); }


} // namespace Durin

namespace Durin::ContentBrowser::Private
{
	auto QueueNativeCommittedUpdates(DContentBrowserDataSource& Source, const FContentChangeBatch& Batch, bool FromDiscovery) -> void
	{
		if (!FromDiscovery)
		{
			if (Cast<DContentBrowserAssetDataSource>(&Source)) (void)GetAssetQueryRevision();
		}
		if (auto* File = Cast<DContentBrowserFileDataSource>(&Source)) RecordContentBrowserFileUpdates(*File, Batch, FromDiscovery);
		QueueCommittedUpdates(Source, Batch, FromDiscovery);
	}
}
