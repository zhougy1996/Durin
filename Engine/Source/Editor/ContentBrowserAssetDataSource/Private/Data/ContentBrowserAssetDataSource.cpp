#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "AssetDefinition/AssetDefinition.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Data/ContentBrowserNativeUpdates.h"
#include "ContentBrowserAssetDataSource.h"
#include "ContentBrowserAssetDataPayload.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "Data/ContentBrowserAssetActions.h"
#include "AssetTools/ContentFolderOperation.h"
#include "Data/ContentBrowserAssetSave.h"
#include "Data/ContentBrowserNativeTransfer.h"
#include "ContentBrowserTransferHelpers.h"
#include "Misc/StringHelper.h"
#include "ObjectTools.h"
#include "AssetTools/DeleteItemsHost.h"
#include "AssetDeleteModel.h"

#include "AssetRegistry/Catalog.h"

#include "Asset/RegistryOperations.h"
#include "Misc/ContentMountPaths.h"
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
		using ContentBrowser::AssetDataSource::Private::FContentBrowserNativeItem;
		using FAssetActions = ContentBrowser::AssetDataSource::Private::FContentBrowserAssetActions;
		using FFolderOperation = ContentStorage::FContentFolderOperation;
		auto NativePolicy(const FContentBrowserOperationContext& Host) -> ContentStorage::FContentMutationPolicy
		{ return {Host.CanMutateContent, Host.ReportDiagnostic, Host.NotifyContentMutation}; }
		auto ResolveItemPath(FContentBrowserNativeItem& Item) -> void
		{
			if (Item.Kind != EContentBrowserItemKind::File && Item.Kind != EContentBrowserItemKind::Folder) return;
			const auto Path = ContentStorage::FContentMountPaths::ResolveMountPath(Item.PhysicalPath);
			if (Path) Item.VirtualPath = Path.VirtualPath;
		}
		auto QueueCommittedUpdates(DContentBrowserDataSource& Source, const FContentChangeBatch& Batch, bool FromDiscovery) -> void
		{
			auto MakeData = [&](const FContentChange& Change, bool Old) {
				FContentBrowserNativeItem Item; Item.PhysicalPath = Old ? Change.OldPhysicalPath : Change.NewPhysicalPath;
				Item.Kind = Change.bDirectory ? EContentBrowserItemKind::Folder : EContentBrowserItemKind::File;
				const auto& AssetPath = Old ? Change.OldAssetPath : Change.NewAssetPath;
				if (!AssetPath.empty() && !Change.bDirectory) { Item.Kind = EContentBrowserItemKind::Asset; Item.VirtualPath = AssetPath; FTopLevelAssetPath Path; if (FTopLevelAssetPath::TryCreate(AssetPath, Path)) Item.PackagePath = Path.GetPackagePath(); }
				ResolveItemPath(Item);
				Item.Name = Change.bDirectory ? std::filesystem::path(Item.VirtualPath).filename().generic_string()
					: std::string(std::filesystem::path(Item.PhysicalPath).stem().generic_string());
				if (!Old && !Change.bDirectory)
				{
					FTopLevelAssetPath Path;
					if (!FTopLevelAssetPath::TryCreate(Item.VirtualPath, Path)) return FContentBrowserItemData{};
					const auto Asset = FindAssetExact(Path);
					if (!Asset) return FContentBrowserItemData{};
					Item.PackagePath = Path.GetPackagePath();
					const auto* AssetSource = Cast<DContentBrowserAssetDataSource>(&Source);
					Item.Name = AssetSource ? AssetSource->GetAssetDisplayName(Path) : std::string(Path.GetAssetName());
					Item.Kind = Asset->IsRedirector() ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset;
					Item.AssetClassName = Asset->AssetClassName;
					Item.RedirectDestination = Asset->RedirectDestination;
					Item.Extension = ".dasset";
					if (Asset.Package)
					{
						Item.PhysicalPath = Asset.Package->PhysicalPath;
						Item.PackageFileSize = Asset.Package->FileSize;
						Item.BulkFileSize = Asset.Package->BulkSegmentExtent;
						Item.FileSize = Item.PackageFileSize + Item.BulkFileSize;
						Item.LastWriteTime = Asset.Package->LastWriteTime;
					}
				}
				return Source.BindItemData(MakeNativeItemDescription(Item));
			};
			auto Queue = [&](const FContentChange& Change) {
				auto Data = MakeData(Change, Change.Kind == EContentChangeKind::Removed);
				auto* Owner = Data.GetOwnerDataSource(); if (!Owner) return;
				if (Change.Kind == EContentChangeKind::Removed) Owner->QueueItemDataUpdate(FContentBrowserItemDataUpdate::MakeItemRemovedUpdate(std::move(Data)));
				else if (Change.Kind == EContentChangeKind::Added) Owner->QueueItemDataUpdate(FContentBrowserItemDataUpdate::MakeItemAddedUpdate(std::move(Data)));
				else if (Change.Kind == EContentChangeKind::Renamed) Owner->QueueItemDataUpdate(FContentBrowserItemDataUpdate::MakeItemMovedUpdate(std::move(Data), MakeData(Change, true).GetVirtualPath()));
				else Owner->QueueItemDataUpdate(FContentBrowserItemDataUpdate::MakeItemModifiedUpdate(std::move(Data)));
			};
			for (const auto& Change : Batch.Changes)
			{
				if (!FromDiscovery && !Change.bDirectory && (!Change.OldAssetPath.empty() || !Change.NewAssetPath.empty())) continue;
				Queue(Change);
			}
			if (Batch.bFullRefresh)
				Source.NotifyItemDataRefreshed();
		}
		auto ScopedHost(DContentBrowserDataSource& Source, FContentBrowserOperationContext Host) -> FContentBrowserOperationContext
		{
			return ScopeOperationContext(Source, std::move(Host), [](DContentBrowserDataSource& Owner, const FContentChangeBatch& Batch) {
				ContentBrowser::AssetDataSource::Private::QueueNativeCommittedUpdates(Owner, Batch);
				ContentStorage::PublishContentDirectoryChanges(Batch, &Owner);
			});
		}

		// Request-local native view; never stored in generic item data.
		struct FNativePayloadView
		{
			FContentBrowserNativeItem Item;
			// Unscoped immutable constraints/services and one operation-local live fence.
			std::shared_ptr<const FContentBrowserOperationContext> Context;
			std::shared_ptr<const FContentBrowserOperationContext> Host;
		};

		auto Payload(const DContentBrowserDataSource& Source, const FContentBrowserItemData& Data) -> std::optional<FNativePayloadView>
		{
			using namespace ContentBrowser::AssetDataSource::Private;
			if (Data.GetOwnerDataSource()!=&Source) return {};
			const auto MakeView=[&Data, &Source](const auto& Payload, EContentBrowserItemKind Kind) {
				const auto Context = Payload.Host ? Payload.Host : std::make_shared<const FContentBrowserOperationContext>();
				const auto Execution = std::make_shared<const FContentBrowserOperationContext>(ScopedHost(*const_cast<DContentBrowserDataSource*>(&Source), *Context));
				return FNativePayloadView{.Item={.Kind=Kind, .Name=Data.GetItemName(), .VirtualPath=Data.GetVirtualPath(), .PhysicalPath=Payload.Location.PhysicalPath}, .Context=Context, .Host=Execution};
			};
			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserAssetFileItemDataPayload>(Data.GetPayload()))
			{
				auto View=MakeView(*P, P->bRedirector ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset);
				View.Item.PackagePath=P->PackagePath; View.Item.AssetClassName=P->AssetClassName; View.Item.Extension=P->Extension;
				View.Item.RedirectDestination=P->RedirectDestination; View.Item.FileSize=P->FileSize;
				View.Item.PackageFileSize=P->PackageFileSize; View.Item.BulkFileSize=P->BulkFileSize; View.Item.LastWriteTime=P->LastWriteTime;
				return View;
			}

			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserAssetFolderItemDataPayload>(Data.GetPayload())) return MakeView(*P, EContentBrowserItemKind::Folder);

			return {};
		}


		auto SaveMode(EContentBrowserItemSaveFlags Flags) -> std::optional<bool>
		{
			const auto Bits = static_cast<uint8>(Flags);
			if (Bits > 3) return {};
			return (Bits & 1) ? false : true;
		}
		auto Destination(std::string_view Path) -> std::string
		{
			return ContentStorage::FContentMountPaths::VirtualToPhysical(std::string(Path));
		}
		auto Bind(DContentBrowserDataSource& Source, const FContentBrowserItemData& Item, std::shared_ptr<const FContentBrowserOperationContext> Host) -> FContentBrowserItemData
		{
			using namespace ContentBrowser::AssetDataSource::Private;
			std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
			std::string Physical;
			auto Clone = [&]<typename T>(std::shared_ptr<const T> Existing) {
				if (!Existing) return false;
				Physical = Existing->Location.PhysicalPath;
				auto Copy = std::make_shared<T>(*Existing);
				Copy->Host = Existing->Host && !Host->IsEmpty() ? std::make_shared<const FContentBrowserOperationContext>(ComposeOperationContext(*Existing->Host, *Host)) : Existing->Host ? Existing->Host : Host;
				Payload = std::move(Copy); return true;
			};

				if (const auto File = std::dynamic_pointer_cast<const FContentBrowserAssetFileItemDataPayload>(Item.GetPayload()))
				{
					Clone(File);
				}
				else Clone(std::dynamic_pointer_cast<const FContentBrowserAssetFolderItemDataPayload>(Item.GetPayload()));

			if (!Payload && Item.GetItemType() == EContentBrowserItemFlags::Type_Folder)
			{
				auto Folder = std::make_shared<FContentBrowserAssetFolderItemDataPayload>();
				Folder->Location.PhysicalPath = ContentStorage::FContentMountPaths::VirtualToPhysical(Item.GetVirtualPath()); Folder->Host = Host;
				Payload = std::move(Folder);
			}
			if (!Payload) return {};
			auto Virtual = Item.GetVirtualPath();
			if (Virtual.empty()) { const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Physical); if (Mount) Virtual = Mount.VirtualPath; }
			return {Source, Item.GetItemFlags() | EContentBrowserItemFlags::Category_Asset, std::move(Virtual), Item.GetItemName(), Item.GetDisplayName(), std::move(Payload)};
		}
		auto BindNative(DContentBrowserDataSource& Source, const FContentBrowserNativeItem& Native, std::shared_ptr<const FContentBrowserOperationContext> Host) -> FContentBrowserItemData
		{
			auto Resolved = Native; ResolveItemPath(Resolved);
			const auto Description = ContentBrowser::AssetDataSource::Private::MakeNativeItemDescription(Resolved, std::move(Host));
			return {Source, Description.GetItemFlags(), Description.GetVirtualPath(), Description.GetItemName(), Description.GetDisplayName(), Description.GetPayload()};
		}
	} // namespace
	DContentBrowserAssetDataSource::DContentBrowserAssetDataSource(const FObjectInitializer& I)
		: Super(I)
	{
	}

	auto DContentBrowserAssetDataSource::GetDataSourceName() const -> FName { return FName("Asset"); }

	auto DContentBrowserAssetDataSource::BindItemData(const FContentBrowserItemData& Item, const FContentBrowserOperationContext& Host) -> FContentBrowserItemData
	{
		DContentBrowserDataSource& Source = *this;
		if (!Source.IsActive() || (Item.GetOwnerKey() != FObjectKey{} && Item.GetOwnerDataSource() != this)) return {};
		if (Host.IsEmpty() && Item.GetOwnerDataSource() == this) return Item;
		return Bind(Source, Item, std::make_shared<const FContentBrowserOperationContext>(Host));
	}

	auto DContentBrowserAssetDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Filter, const std::function<bool(FContentBrowserItemData&&)>& Callback) -> void
	{
		DContentBrowserDataSource& Source = *this;
		if (!Source.IsActive()) return;
		auto Policy = std::make_shared<const FContentBrowserOperationContext>();
		const auto Found = Filter.CompiledFilters.find(&Source);
		if (Found == Filter.CompiledFilters.end()) return;
		if (!Found->second) return;
		FContentBrowserDataCompiledFilter Current;
		Source.CompileFilter(Filter.Path, Filter.Filter, Current);
		const auto Snapshot = Current.CompiledFilters.at(&Source)->Capture({}, {});
		for (auto Item : Snapshot.Items)
		{
			if (!Source.IsActive()) return;
			auto Bound = Bind(Source, Item, Policy);
			if (!Bound.GetOwnerDataSource()) continue;
			if (!Source.DoesItemPassFilter(Bound, Filter)) continue;
			if (!Callback(std::move(Bound))) return;
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
		auto AssetTypeDisplayName(const ContentBrowser::AssetDataSource::Private::FContentBrowserAssetFileItemDataPayload& File) -> std::string
		{
			if (File.bRedirector) return "Redirector";
			if (const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(File.AssetClassName)) { const auto Name = Definition->GetAssetDisplayName(); if (!Name.empty()) return Name; }
			const auto Separator=File.AssetClassName.rfind("::");
			auto Name=Separator==std::string::npos ? File.AssetClassName : File.AssetClassName.substr(Separator+2);
			if (Name.starts_with('D') && Name.size()>1) Name.erase(Name.begin());
			return Name;
		}
	}
	auto DContentBrowserAssetDataSource::GetItemAttribute(const FContentBrowserItemData& Data, bool IncludeMetaData, std::string_view Key, FContentBrowserItemDataAttributeValue& Out) const -> bool
	{
		using namespace ContentBrowser::AssetDataSource::Private;
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

	auto DContentBrowserAssetDataSource::GetItemAttributes(const FContentBrowserItemData& Data, bool IncludeMetaData, FContentBrowserItemDataAttributeValues& Out) const -> bool
	{
		using namespace ContentBrowserItemAttributes;
		return CollectAttributes(*this, Data, IncludeMetaData, {ItemTypeName, ItemTypeDisplayName, Class, ItemDiskSize, AssetPath, PackagePath, Extension, IsRedirector, RedirectDestination, PackageFileSize, BulkFileSize, LastWriteTime}, Out);
	}
	auto DContentBrowserAssetDataSource::GetItemPhysicalPath(const FContentBrowserItemData& Data, std::string& Out) const -> bool
	{
		using namespace ContentBrowser::AssetDataSource::Private;
		Out.clear();
		if (Data.GetOwnerDataSource()!=this) return false;
		if (const auto File=std::dynamic_pointer_cast<const FContentBrowserAssetFileItemDataPayload>(Data.GetPayload())) Out=File->Location.PhysicalPath;
		else if (const auto Folder=std::dynamic_pointer_cast<const FContentBrowserAssetFolderItemDataPayload>(Data.GetPayload())) Out=Folder->Location.PhysicalPath;
		return !Out.empty();
	}

	auto DContentBrowserAssetDataSource::CanEditItem(const FContentBrowserItemData& Data, std::string*) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && (P->Item.Kind == EContentBrowserItemKind::Asset || P->Item.Kind == EContentBrowserItemKind::Redirector) && bool(P->Host->OpenAsset);
	}
	auto DContentBrowserAssetDataSource::EditItem(const FContentBrowserItemData& Data) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		auto P = Payload(*this, Data);
		if (!P || !CanEditItem(Data)) return false;
		FSoftObjectPath Path;
		if (!FSoftObjectPath::TryCreate(P->Item.VirtualPath, Path)) return false;
		const auto Resolved = ResolveAssetObjectPathForOperation(Path);
		if (!Resolved || !Resolved.FinalAssetData) return false;
		auto* Editor = GetOwningEditor();
		auto* Editors = Editor ? Editor->GetEditorSubsystem<DAssetEditorSubsystem>() : nullptr;
		const auto* Definition = FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Resolved.FinalAssetData->AssetClassName);
		if (!Definition) return P->Host->OpenAsset(Resolved.FinalPath.ToString(), Resolved.FinalAssetData->AssetClassName);
		FStrongObjectPtr DefinitionLifetime(const_cast<DAssetDefinition*>(Definition));
		FAssetActivateArgs Args{.AssetEditors = Editors,
			.Assets = {{Resolved.FinalPath, Resolved.FinalAssetData->AssetClassName}}, .ActivationMethod = EAssetActivationMethod::Opened};
		Args.Assets = Definition->PrepareToActivateAssets(Args);
		if (Args.Assets.empty()) return true;
		FAssetOpenArgs OpenArgs{.AssetEditors = Editors, .Assets = Args.Assets};
		for (const auto& Target : Args.Assets)
		{
			auto* Asset = OpenArgs.LoadAsset(Target);
			if (!Asset) return false;
		}
		if (!IsActive()) return false;
		if (Definition->ActivateAssets(Args) == EAssetCommandResult::Handled) return true;
		bool Opened = true;
		for (const auto& Target : Args.Assets)
			Opened = P->Host->OpenAsset(Target.AssetPath.ToString(), Target.AssetClassName) && Opened;
		return Opened;
	}

	auto DContentBrowserAssetDataSource::CanDuplicateItem(const FContentBrowserItemData& Data, std::string* Error) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && FAssetActions(*P->Host).CanDuplicateItem(P->Item, Error);
	}
	auto DContentBrowserAssetDataSource::DuplicateItem(const FContentBrowserItemData& Data, FContentBrowserItemData& Out) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const auto Item = Data;
		Out = {};
		auto P = Payload(*this, Item);
		if (!P) return false;
		FContentBrowserNativeItem Native;
		const bool Result = FAssetActions(*P->Host).DuplicateItem(P->Item, Native);
		if (!Native.PhysicalPath.empty() && Item.GetOwnerDataSource() == this) Out = BindNative(*this, Native, P->Context);
		return Result;
	}

	auto DContentBrowserAssetDataSource::CanCreateFolder(std::string_view Path, std::string* Error) const -> bool
	{
		const DContentBrowserDataSource& Source = *this;
		if (!Source.IsActive()) return false;
		const std::filesystem::path Virtual(Path);
		FFolderOperation Context(NativePolicy(ScopedHost(*const_cast<DContentBrowserDataSource*>(&Source), {})));
		return Context.CanCreateFolder(Destination(Virtual.parent_path().generic_string()), Virtual.filename().string(), Error);
	}

	auto DContentBrowserAssetDataSource::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserOperationContext& Host) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		DContentBrowserDataSource& Source = *this;
		if (!Source.IsActive()) return false;
		const std::filesystem::path Virtual(Path);
		const auto Directory = Destination(Virtual.parent_path().generic_string());
		auto RawPolicy = std::make_shared<const FContentBrowserOperationContext>(Host);
		auto Policy = std::make_shared<const FContentBrowserOperationContext>(ScopedHost(Source, Host));
		auto Operations = std::make_shared<FFolderOperation>(NativePolicy(*Policy));
		if (!Operations->CanCreateFolder(Directory, Virtual.filename().string())) return false;
		FContentBrowserNativeItem Native{.Kind = EContentBrowserItemKind::Folder, .Name = Virtual.filename().string(), .VirtualPath = std::string(Path), .PhysicalPath = Destination(Path)};
		auto Data = BindNative(Source, Native, RawPolicy);
		Data = FContentBrowserItemData(Source, Data.GetItemFlags() | EContentBrowserItemFlags::Temporary_Creation, Data.GetVirtualPath(), Data.GetItemName(), Data.GetDisplayName(), Data.GetPayload());
		Out = {Data, [Data, Operations, Directory](std::string_view Name, std::string* Error) { return Data.GetOwnerDataSource() && Operations->CanCreateFolder(Directory, Name, Error); }, [Data, Operations, Directory, RawPolicy](std::string_view Name, FContentBrowserItemData& Result) {
			auto* Owner = Data.GetOwnerDataSource();
			if (!Owner) return false;
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Owner);
			std::string Created;
			if (!Operations->CreateFolder(Directory, Name, Created)) return false;
			Result = {};
			if (Data.GetOwnerDataSource()) Result = BindNative(*Owner, {.Kind = EContentBrowserItemKind::Folder, .Name = std::string(Name), .PhysicalPath = std::move(Created)}, RawPolicy);
			return true;
		}};
		return true;
	}

	auto DContentBrowserAssetDataSource::FixUpRedirectors(std::span<const FTopLevelAssetPath> Paths, const FContentBrowserOperationContext& Host) -> bool
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
		return P && Mode && ContentBrowser::AssetDataSource::Private::FContentBrowserAssetSaveOperation(*P->Host).CanSaveItem(P->Item, *Mode, Error);
	}
	auto DContentBrowserAssetDataSource::BulkSaveItems(std::span<const FContentBrowserItemData> Data, EContentBrowserItemSaveFlags Flags) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const auto Mode = SaveMode(Flags);
		std::vector<FContentBrowserNativeItem> Items;
		std::vector<std::shared_ptr<const FContentBrowserOperationContext>> Policies;
		std::vector<FStrongObjectPtr> LoadedRoots;
		for (const auto& Entry : Data)
		{
			auto P = Payload(*this, Entry);
			if (!P || P->Item.Kind != EContentBrowserItemKind::Asset || !Mode)
			{
				return false;
			}
			Policies.push_back(P->Host);
			if (static_cast<uint8>(Flags) & static_cast<uint8>(EContentBrowserItemSaveFlags::SaveOnlyIfLoaded))
			{
				auto* Package = FindResidentPackage(P->Item.PackagePath);
				if (!Package)
				{
					continue;
				}
				LoadedRoots.emplace_back(Package);
				for (auto* Export : Package->GetTopLevelAssets()) LoadedRoots.emplace_back(Export);
			}
			Items.push_back(P->Item);
		}
		if (Items.empty()) return false;
		auto Host = *Policies.front();
		Host.CanMutateContent = [Policies] {
			for (const auto& Policy : Policies) if (Policy->CanMutateContent && !Policy->CanMutateContent()) return false;
			return true;
		};
		return ContentBrowser::AssetDataSource::Private::FContentBrowserAssetSaveOperation(std::move(Host)).BulkSaveItems(Items, *Mode);
	}	namespace
	{
		auto RenameTarget(const FContentBrowserNativeItem& Item, std::string_view Name) -> std::filesystem::path
		{
			auto Target = std::filesystem::path(Item.PhysicalPath).parent_path() / Name;
			if (Item.Kind == EContentBrowserItemKind::Asset) Target += ".dasset";
			return Target;
		}
		auto ValidateRenameTarget(const FContentBrowserNativeItem& Item, std::string_view Name, std::string* Error, bool MergeDirectory = false) -> bool
		{
			if (Name.empty() || Name == "." || Name == ".." || Name.find_first_of("/\\:*?\"<>|") != std::string_view::npos || Name.back() == '.' || Name.back() == ' ')
			{ if (Error) *Error = "Enter a valid name without path separators."; return false; }
			const auto Target = RenameTarget(Item, Name);
			if (ContentBrowser::Transfer::Same(Target, Item.PhysicalPath)) return true;
			if (!ContentBrowser::Transfer::SafePath(Target, true, Error)) return false;
			std::error_code Ec;
			if ((std::filesystem::exists(Target, Ec) && !(MergeDirectory && Item.Kind == EContentBrowserItemKind::Folder && std::filesystem::is_directory(Target, Ec))) || Ec) { if (Error) *Error = "The rename destination is already occupied or unavailable."; return false; }
			return true;
		}
		auto CanTransferAsset(const ContentBrowser::AssetDataSource::Private::FNativeTransferInput& Input, std::string_view Destination, bool Move, std::string* Error) -> bool
		{
			if (Error) Error->clear();
			FTopLevelAssetPath Identity;
			if (!FTopLevelAssetPath::TryCreate(Input.Item.VirtualPath, Identity)) return false;
			const auto Asset = FindAssetExact(Identity);
			if (!Asset || Asset->IsRedirector()) { if (Error) *Error = "The asset is unavailable or is a redirector."; return false; }
			const auto Target = std::filesystem::path(Destination) / (std::string(Identity.GetAssetName()) + ".dasset");
			using namespace ContentBrowser::Transfer;
			if (!Admitted(std::span{&Input, 1}, Error) || !SafePath(Input.Item.PhysicalPath, Move, Error) || !SafePath(Target, true, Error)) return false;
			std::error_code Ec;
			if (!std::filesystem::is_directory(Destination, Ec) || Ec || Same(Target, Input.Item.PhysicalPath) || std::filesystem::exists(Target, Ec) || Ec)
			{ if (Error) *Error = "The destination is unavailable or occupied."; return false; }
			return Admitted(std::span{&Input, 1}, Error);
		}
		auto ExecuteNativeTransfer(DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, std::string_view Dest, bool Move) -> bool
		{
			TStrongObjectPtr<DContentBrowserDataSource> Lifetime(&Source);
			const std::vector<FContentBrowserItemData> Snapshot(Data.begin(), Data.end());
			const std::string Target(Dest);
			std::vector<ContentBrowser::AssetDataSource::Private::FNativeTransferInput> Inputs;
			for (const auto& Item : Snapshot)
			{
				const auto P = Payload(Source, Item); if (!P) continue;
				Inputs.push_back({P->Item, P->Host});
			}
			if (Inputs.empty()) return false;
			return ContentBrowser::AssetDataSource::Private::ExecuteAssetTransfer(std::move(Inputs), Destination(Target), Move);
		}
	}
	auto DContentBrowserAssetDataSource::RenameItem(const FContentBrowserItemData& Input, std::string_view Name, FContentBrowserItemData& Out) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const auto Item = Input;
		const std::string NewName(Name);
		Out = {};
		const auto P = Payload(*this, Item); if (!P) return false;
		std::string Error;
		if (!ValidateRenameTarget(P->Item, NewName, &Error, true)) { if (P->Host->ReportDiagnostic) P->Host->ReportDiagnostic(ELogLevel::Error, Error); return false; }
		const auto Target = RenameTarget(P->Item, NewName);
		if (ContentBrowser::Transfer::Same(Target, P->Item.PhysicalPath))
		{
			FTopLevelAssetPath Asset;
			if (P->Item.Kind != EContentBrowserItemKind::Asset || (FTopLevelAssetPath::TryCreate(P->Item.VirtualPath, Asset) && Asset.GetAssetName() == NewName)) { Out = Item; return true; }
		}
		const std::vector<ContentBrowser::AssetDataSource::Private::FNativeTransferInput> Inputs{{P->Item, P->Host}};
		const bool Success = ContentBrowser::AssetDataSource::Private::ExecuteAssetTransfer(Inputs, std::filesystem::path(P->Item.PhysicalPath).parent_path().generic_string(), true, NewName);
		if (!Success || Item.GetOwnerDataSource() != this) return Success;
		auto Renamed = P->Item; Renamed.Name = NewName; Renamed.PhysicalPath = Target.generic_string();
		Renamed.VirtualPath = ContentStorage::FContentMountPaths::ResolveMountPath(Renamed.PhysicalPath).VirtualPath;
		if (Renamed.Kind == EContentBrowserItemKind::Asset)
		{
			FTopLevelAssetPath Old;
			if (FTopLevelAssetPath::TryCreate(P->Item.VirtualPath, Old))
			{
				const auto Source = Old.GetPackagePath().ToString();
				FPackagePath TargetPackage;
				if (FPackagePath::TryCreate(Source.substr(0, Source.find_last_of('/') + 1) + NewName, TargetPackage))
					if (auto* Package = FindResidentPackage(TargetPackage))
						if (auto* Object = Package->FindTopLevelAsset(FName(NewName)))
						{ Renamed.PackagePath = Package->GetPackagePathIdentity(); Renamed.VirtualPath = Object->GetObjectPath(); }
			}
		}
		Out = BindNative(*this, Renamed, P->Context);
		return Success;
	}
	auto DContentBrowserAssetDataSource::CopyItem(const FContentBrowserItemData& Item, std::string_view Destination) -> bool
	{ return BulkCopyItems(std::span{&Item, 1}, Destination); }
	auto DContentBrowserAssetDataSource::BulkCopyItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> bool
	{ return ExecuteNativeTransfer(*this, Items, Destination, false); }
	auto DContentBrowserAssetDataSource::MoveItem(const FContentBrowserItemData& Item, std::string_view Destination) -> bool
	{ return BulkMoveItems(std::span{&Item, 1}, Destination); }
	auto DContentBrowserAssetDataSource::BulkMoveItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> bool
	{ return ExecuteNativeTransfer(*this, Items, Destination, true); }

	auto DContentBrowserAssetDataSource::CanRenameItem(const FContentBrowserItemData& Item, std::optional<std::string_view> Name, std::string* Error) const -> bool
	{
		const auto CapturedItem = Item;
		const std::optional<std::string> CapturedName = Name ? std::optional(std::string(*Name)) : std::nullopt;
		const auto P = Payload(*this, CapturedItem);
		if (!P) return false;
		const ContentBrowser::AssetDataSource::Private::FNativeTransferInput Input{P->Item, P->Host};
		if (P->Item.Kind == EContentBrowserItemKind::Asset)
		{
			FTopLevelAssetPath Identity;
			if (!FTopLevelAssetPath::TryCreate(P->Item.VirtualPath, Identity) || !FindAssetExact(Identity)
				|| !ContentBrowser::Transfer::Admitted(std::span{&Input, 1}, Error) || !ContentBrowser::Transfer::SafePath(P->Item.PhysicalPath, true, Error)) return false;
		}
		else if (!ContentBrowser::Transfer::CanRename(Input, Error)) return false;
		if (CapturedItem.GetOwnerDataSource() != this) return false;
		if (!CapturedName) return true;
		return ValidateRenameTarget(P->Item, *CapturedName, Error);
	}
	auto DContentBrowserAssetDataSource::CanCopyItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		const std::string CapturedDestination(Dest);
		const auto CapturedItem = Item;
		const auto P = Payload(*this, CapturedItem);
		if (!P) return false;
		const ContentBrowser::AssetDataSource::Private::FNativeTransferInput Input{P->Item, P->Host};
		return (P->Item.Kind == EContentBrowserItemKind::Asset ? CanTransferAsset(Input, Destination(CapturedDestination), false, Error) : ContentBrowser::Transfer::CanTransfer(Input, Destination(CapturedDestination), false, Error)) && CapturedItem.GetOwnerDataSource() == this;
	}
	auto DContentBrowserAssetDataSource::CanMoveItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		const std::string CapturedDestination(Dest);
		const auto CapturedItem = Item;
		const auto P = Payload(*this, CapturedItem);
		if (!P) return false;
		const ContentBrowser::AssetDataSource::Private::FNativeTransferInput Input{P->Item, P->Host};
		return (P->Item.Kind == EContentBrowserItemKind::Asset ? CanTransferAsset(Input, Destination(CapturedDestination), true, Error) : ContentBrowser::Transfer::CanTransfer(Input, Destination(CapturedDestination), true, Error, true)) && CapturedItem.GetOwnerDataSource() == this;
	}

	auto DContentBrowserAssetDataSource::CanDeleteItem(const FContentBrowserItemData& Item, std::string* Error) const -> bool
	{
		const DContentBrowserDataSource& Source = *this;
		auto P = Payload(Source, Item);
		if (!P) return false;
		if (!ContentStorage::CanModifyContent(NativePolicy(*P->Host), Error)) return false;
		const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(P->Item.PhysicalPath);
		const bool Allowed = Mount && Mount.Mount->bContentWritable
							 && !P->Item.PhysicalPath.empty()
							 && std::filesystem::path(P->Item.PhysicalPath).lexically_normal() != Mount.Mount->GetContentDir().lexically_normal();
		if (!Allowed && Error) *Error = "Deletion requires an item inside a writable content mount.";
		return Allowed;
	}

	auto DContentBrowserAssetDataSource::DeleteItem(const FContentBrowserItemData& Item) -> bool
	{ return BulkDeleteItems(std::span(&Item, 1)); }

	auto DContentBrowserAssetDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData> Data) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Data.begin(), Data.end());
		FAssetDeleteModel Model(std::span<DObject* const>{});
		std::vector<std::shared_ptr<const FContentBrowserOperationContext>> Policies;
		for (const auto& Item : Snapshot)
		{
			if (!CanDeleteItem(Item)) return false;
			const auto View = Payload(*this, Item);
			if (!View) return false;
			Policies.push_back(View->Host);
			if (const auto Asset = std::dynamic_pointer_cast<const ContentBrowser::AssetDataSource::Private::FContentBrowserAssetFileItemDataPayload>(Item.GetPayload()))
			{
				auto* Package = FindResidentPackage(Asset->AssetPath.GetPackagePath());
				if (!Package) Package = LoadPackage(Asset->AssetPath.GetPackagePath());
				auto* Object = Package ? Package->FindTopLevelAsset(FName(Asset->AssetPath.GetAssetName())) : nullptr;
				if (!Object) return false;
				Model.AddObjectToDelete(Object);
			}
			else
			{
				const auto Root = std::filesystem::path(View->Item.PhysicalPath).lexically_normal();
				for (const auto& [Identity, Asset] : CaptureAssetQuerySnapshot().Assets)
				{
					const auto Resolved = FMountPaths::ResolveAssetPath(Identity.GetPackagePath().GetView());
					if (!Resolved) continue;
					std::filesystem::path Relative;
					if (!FPaths::TryMakeLexicalRelativePath(Resolved->PhysicalPath.generic_string() + ".dasset", Root, Relative)) continue;
					auto* Package = FindResidentPackage(Identity.GetPackagePath());
					if (!Package) Package = LoadPackage(Identity.GetPackagePath());
					auto* Object = Package ? Package->FindTopLevelAsset(FName(Identity.GetAssetName())) : nullptr;
					if (!Object) return false;
					Model.AddObjectToDelete(Object);
				}
			}
		}
		return ObjectTools::Private::DeleteObjects(Model, [&] {
			for (const auto& Policy : Policies) if (Policy->CanMutateContent && !Policy->CanMutateContent()) return false;
			for (const auto& Item : Snapshot) if (Item.GetOwnerDataSource() != this) return false;
			return true;
		}) != 0;
	}
} // namespace Durin

namespace Durin::ContentBrowser::AssetDataSource::Private
{
	auto QueueNativeCommittedUpdates(DContentBrowserDataSource& Source, const FContentChangeBatch& Batch, bool FromDiscovery) -> void
	{
				QueueCommittedUpdates(Source, Batch, FromDiscovery);
	}
}
