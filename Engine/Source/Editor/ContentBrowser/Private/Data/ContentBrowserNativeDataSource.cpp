#include "ContentBrowserNativeDataSource.h"
#include "ContentBrowserDataSubsystem.h"
#include "Data/ContentBrowserNativeItemPayloads.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "Data/ContentBrowserRequestContext.h"

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
		using FOperations = ContentBrowser::Private::FContentBrowserRequestContext;
		auto ResolveItemPath(FContentBrowserNativeItem& Item) -> void
		{
			if (Item.Kind != EContentBrowserItemKind::File && Item.Kind != EContentBrowserItemKind::Folder) return;
			const auto Path = ContentBrowser::Private::FContentBrowserPaths::ResolveMountPath(Item.PhysicalPath);
			if (Path) Item.VirtualPath = Path.VirtualPath;
		}
		auto QueueCommittedUpdates(DContentBrowserDataSource& Source, const FContentChangeBatch& Batch) -> void
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
				QueueChange(Change);
				if (Change.bDirectory)
					if (auto* Service = Cast<DContentBrowserDataSubsystem>(Source.GetOuter()))
						if (auto* Peer = Cast<DContentBrowserNativeDataSource>(Service->GetActiveDataSource(FName(Cast<DContentBrowserAssetDataSource>(&Source) ? "File" : "Asset"))))
							QueueChange(Change, Peer);
			}
			if (Batch.bFullRefresh)
				for (const auto& Mount : ContentBrowser::Private::FContentBrowserPaths::CaptureMounts())
				{
					FContentBrowserNativeItem Folder{.Kind = EContentBrowserItemKind::Folder, .VirtualPath = Mount.VirtualRoot, .PhysicalPath = Mount.PhysicalRoot};
					Queue(FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(Source.CreateItemData(ContentBrowser::Private::MakeNativeItem(Folder, Cast<DContentBrowserAssetDataSource>(&Source) != nullptr))));
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
		// Transient bridge for the legacy executor; never stored in an item payload.
		struct FNativePayloadView
		{
			FContentBrowserNativeItem Item;
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

		auto Gather(const DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, std::vector<FContentBrowserNativeItem>& Items) -> std::shared_ptr<FOperations>
		{
			std::vector<std::shared_ptr<FOperations>> Contexts;
			std::vector<std::shared_ptr<const FContentBrowserHostContext>> Policies;
			if (Data.empty()) return {};
			for (const auto& Entry : Data)
			{
				auto* Owner = Entry.GetOwnerDataSource();
				auto* Native = Owner ? Cast<DContentBrowserNativeDataSource>(Owner) : nullptr;
				auto P = Native && Native->GetOuter() == Source.GetOuter() ? Payload(*Native, Entry) : std::optional<FNativePayloadView>{};
				if (!P) return {};
				if (!std::ranges::contains(Policies, P->Host))
				{
					Policies.push_back(P->Host);
					Contexts.push_back(std::make_shared<FOperations>(*P->Host));
				}
				const bool Alias = P->Item.Kind == EContentBrowserItemKind::Folder && std::ranges::any_of(Items, [&](const auto& Existing) {
					std::filesystem::path Relative;
					return Existing.Kind == EContentBrowserItemKind::Folder && FPaths::TryMakeLexicalRelativePath(Existing.PhysicalPath, P->Item.PhysicalPath, Relative) && Relative.empty();
				});
				if (!Alias) Items.push_back(P->Item);
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
		auto BindNative(DContentBrowserDataSource& Source, const FContentBrowserNativeItem& Native, std::shared_ptr<const FContentBrowserHostContext> Host) -> FContentBrowserItemData
		{
			auto Resolved = Native; ResolveItemPath(Resolved);
			return Bind(Source, ContentBrowser::Private::MakeNativeItem(Resolved, Cast<DContentBrowserAssetDataSource>(&Source) != nullptr), std::move(Host));
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
	auto DContentBrowserNativeDataSource::AreItemsEquivalentForOperations(const FContentBrowserItemData& A, const FContentBrowserItemData& B) const -> bool
	{
		if (Super::AreItemsEquivalentForOperations(A, B)) return true;
		auto* Other = Cast<DContentBrowserNativeDataSource>(B.GetOwnerDataSource());
		if (!Other || Other->GetOuter() != GetOuter() || A.GetItemDataKey() != B.GetItemDataKey()) return false;
		const auto First = Payload(*this, A), Second = Payload(*Other, B);
		if (!First || !Second || First->Item.Kind != EContentBrowserItemKind::Folder || Second->Item.Kind != EContentBrowserItemKind::Folder) return false;
		std::filesystem::path Relative;
		return FPaths::TryMakeLexicalRelativePath(First->Item.PhysicalPath, Second->Item.PhysicalPath, Relative) && Relative.empty();
	}

	auto DContentBrowserNativeDataSource::CreateItemData(const FContentBrowserItem& Item, const FContentBrowserHostContext& Host) -> FContentBrowserItemData
	{
		if (!IsActive()) return {};
		return Bind(*this, Item, std::make_shared<const FContentBrowserHostContext>(ScopedHost(*this, Host)));
	}
	auto DContentBrowserNativeDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Filter, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserHostContext& Host) -> void
	{
		if (!IsActive()) return;
		const auto Generation = GetActivationGeneration();
		const auto MountRevision = FMountPaths::GetMountRegistryRevision();
		auto Policy = std::make_shared<const FContentBrowserHostContext>(ScopedHost(*this, Host));
		const auto Found = Filter.CompiledFilters.find(this);
		if (Found == Filter.CompiledFilters.end()) return;
		if (Found->second.Generation != Generation || Found->second.MountRevision != MountRevision
			|| Found->second.DiscoveryRevision != GetDiscoveryRevision()) return;
		if (!Found->second.Data) return;
		const auto Snapshot = Found->second.Data->Capture({}, {});
		for (auto Item : Snapshot.Items)
		{
			if (!IsActive() || Generation != GetActivationGeneration() || MountRevision != FMountPaths::GetMountRegistryRevision()) return;
			auto Bound = Bind(*this, Item, Policy);
			if (!Bound.GetOwnerDataSource()) continue;
			if (Filter.Filter.Kind) { const auto Native = Payload(*this, Bound); if (!Native || Native->Item.Kind != *Filter.Filter.Kind) continue; }
			Item.InternalItems = {std::move(Bound)};
			if (!Callback(std::move(Item))) return;
		}
	}
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
		FContentBrowserNativeItem Native;
		const bool Result = FOperations(*P->Host).DuplicateItem(P->Item, Native);
		Out = {};
		if (!Native.PhysicalPath.empty() && IsActive()) Out.InternalItems = {BindNative(*this, Native, P->Host)};
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
		FContentBrowserNativeItem Native{.Kind = EContentBrowserItemKind::Folder, .Name = Virtual.filename().string(), .VirtualPath = std::string(Path), .PhysicalPath = Destination(Path)};
		auto Data = BindNative(*this, Native, Policy);
		Data = FContentBrowserItemData(*this, Data.GetItemFlags() | EContentBrowserItemFlags::Temporary_Creation, Data.GetVirtualPath(), Data.GetItemName(), Data.GetDisplayName(), Data.GetPayload());
		FContentBrowserItem Item{.InternalItems = {Data}};
		Out = {std::move(Item), [Data, Operations, Directory](std::string_view Name, std::string* Error) { return Data.GetOwnerDataSource() && Operations->CanCreateFolder(Directory, Name, Error); }, [Data, Operations, Directory, Policy](std::string_view Name, FContentBrowserItem& Result) {
			auto* Owner = Data.GetOwnerDataSource();
			if (!Owner) return false;
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Owner);
			FContentBrowserNativeItem Created;
			if (!Operations->CreateFolder(Directory, Name, Created)) return false;
			Result = {};
			if (Data.GetOwnerDataSource()) Result.InternalItems = {BindNative(*Owner, Created, Policy)};
			return true;
		}};
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
		std::vector<FContentBrowserNativeItem> Items;
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
		FContentBrowserNativeItem Native;
		if (!P || !FOperations(*P->Host).RenameItem(P->Item, Name, Native)) return false;
		Out = {};
		if (IsActive()) Out.InternalItems = {BindNative(*this, Native, P->Host)};
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
		std::vector<FContentBrowserNativeItem> Items;
		auto Operations = Gather(*this, Data, Items);
		if (!Operations || Destination(Dest).empty()) return false;
		const bool Result = Operations->BulkCopyItems(Items, Destination(Dest));
		return Result;
	}
	auto DContentBrowserNativeDataSource::BulkMoveItems(std::span<const FContentBrowserItemData> Data, std::string_view Dest) -> bool
	{
		std::vector<FContentBrowserNativeItem> Items;
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
		std::vector<FContentBrowserNativeItem> Items;
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
