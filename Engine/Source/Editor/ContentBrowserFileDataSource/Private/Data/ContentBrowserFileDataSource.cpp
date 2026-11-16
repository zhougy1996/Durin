
#include "Data/ContentBrowserNativeUpdates.h"
#include "ContentBrowserFileDataSource.h"
#include "ContentBrowserFileDataPayload.h"
#include "Data/ContentBrowserNativeDescription.h"
#include "AssetTools/ContentFolderOperation.h"
#include "Data/ContentBrowserNativeTransfer.h"
#include "ContentBrowserTransferHelpers.h"
#include "Misc/StringHelper.h"
#include "AssetTools/FileDeletionRequest.h"

#include "AssetRegistry/Catalog.h"

#ifdef _WIN32
	#include <shellapi.h>
#endif
#include "Misc/ContentMountPaths.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "Editor/EditorEngine.h"
#include "DObject/StrongObjectPtr.h"
namespace Durin
{
	using namespace ContentBrowser;
	namespace
	{
		using ContentBrowser::FileDataSource::Private::FContentBrowserNativeItem;
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
				ResolveItemPath(Item);
				const auto Physical = std::filesystem::path(Item.PhysicalPath);
				Item.Name = std::filesystem::path(Item.VirtualPath).filename().generic_string();
				if (!Change.bDirectory) Item.Extension = Physical.extension().generic_string();
				if (!Old && !Change.bDirectory)
				{
					std::error_code Error;
					const auto Status = std::filesystem::symlink_status(Physical, Error);
					if (Error || !std::filesystem::is_regular_file(Status)) return FContentBrowserItemData{};
					Item.FileSize = std::filesystem::file_size(Physical, Error);
					if (!Error) Item.LastWriteTime = std::filesystem::last_write_time(Physical, Error);
					if (Error) return FContentBrowserItemData{};
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

				Queue(Change);
			}
			if (Batch.bFullRefresh)
				Source.NotifyItemDataRefreshed();
		}
		auto ScopedHost(DContentBrowserDataSource& Source, FContentBrowserOperationContext Host) -> FContentBrowserOperationContext
		{
			return ScopeOperationContext(Source, std::move(Host), [](DContentBrowserDataSource& Owner, const FContentChangeBatch& Batch) {
				ContentBrowser::FileDataSource::Private::QueueNativeCommittedUpdates(Owner, Batch);
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
			using namespace ContentBrowser::FileDataSource::Private;
			if (Data.GetOwnerDataSource()!=&Source) return {};
			const auto MakeView=[&Data, &Source](const auto& Payload, EContentBrowserItemKind Kind) {
				const auto Context = Payload.Host ? Payload.Host : std::make_shared<const FContentBrowserOperationContext>();
				const auto Execution = std::make_shared<const FContentBrowserOperationContext>(ScopedHost(*const_cast<DContentBrowserDataSource*>(&Source), *Context));
				return FNativePayloadView{.Item={.Kind=Kind, .Name=Data.GetItemName(), .VirtualPath=Data.GetVirtualPath(), .PhysicalPath=Payload.Location.PhysicalPath}, .Context=Context, .Host=Execution};
			};

			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserFileItemDataPayload>(Data.GetPayload()))
			{
				auto View=MakeView(*P, EContentBrowserItemKind::File);
				View.Item.Extension=P->Extension; View.Item.FileSize=P->FileSize; View.Item.LastWriteTime=P->LastWriteTime;
				return View;
			}

			if (const auto P=std::dynamic_pointer_cast<const FContentBrowserFolderItemDataPayload>(Data.GetPayload())) return MakeView(*P, EContentBrowserItemKind::Folder);
			return {};
		}

		// Ownership of the selected file is a capability restriction. Descendant
		// ownership remains part of the full transfer plan, not this query.
		auto CanOperateSelectedFile(const FContentBrowserNativeItem& Item, std::string* Error) -> bool
		{
			if (Item.Kind == EContentBrowserItemKind::Folder) return true;
			const auto Extension = StringUtils::FoldAscii(std::filesystem::path(Item.PhysicalPath).extension().generic_string());
			if (Extension == ".dasset" || Extension == ".dbulk")
			{ if (Error) *Error = "Package and bulk files must be operated on through their owning package."; return false; }
			FAssetCompanionOwnership Ownership;
			std::optional<UnifiedError::FError> Failure;
			if (!QueryAssetCompanionOwnership(Item.PhysicalPath, Ownership, &Failure))
			{ if (Error) *Error = Failure ? Failure->ToString() : "Could not inspect file ownership."; return false; }
			if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
			{ if (Error) *Error = "Transfer or rename the owning package instead of its managed companion."; return false; }
			return true;
		}
		auto GatherDeletion(const DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, std::vector<FContentBrowserNativeItem>& Items) -> std::shared_ptr<ContentStorage::FFileDeletionRequest>
		{
			std::vector<std::shared_ptr<const FContentBrowserOperationContext>> Policies;
			bool Assets = false, Files = false;
			for (const auto& Entry : Data)
			{
				auto* Owner = Entry.GetOwnerDataSource();
				const auto P = Owner == &Source ? Payload(*Owner, Entry) : std::optional<FNativePayloadView>{};
				if (!P) return {};
				Assets |= P->Item.Kind == EContentBrowserItemKind::Folder;
				Files |= true;
				Policies.push_back(P->Host);
				if (std::ranges::none_of(Items, [&](const auto& Item) { return std::filesystem::path(Item.PhysicalPath).lexically_normal() == std::filesystem::path(P->Item.PhysicalPath).lexically_normal(); })) Items.push_back(P->Item);
			}
			if (Policies.empty()) return {};
			auto Host = *Policies.front();
			Host.CanMutateContent = [Policies] { for (const auto& Policy : Policies) if (Policy->CanMutateContent && !Policy->CanMutateContent()) return false; return true; };
			return std::make_shared<ContentStorage::FFileDeletionRequest>(ContentStorage::FContentMutationPolicy{.CanMutateContent=Host.CanMutateContent, .ReportDiagnostic=Host.ReportDiagnostic, .NotifyContentMutation=Host.NotifyContentMutation}, Assets, Files);
		}

		auto Destination(std::string_view Path) -> std::string
		{
			return ContentStorage::FContentMountPaths::VirtualToPhysical(std::string(Path));
		}
		auto Bind(DContentBrowserDataSource& Source, const FContentBrowserItemData& Item, std::shared_ptr<const FContentBrowserOperationContext> Host) -> FContentBrowserItemData
		{
			using namespace ContentBrowser::FileDataSource::Private;
			std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
			std::string Physical;
			auto Clone = [&]<typename T>(std::shared_ptr<const T> Existing) {
				if (!Existing) return false;
				Physical = Existing->Location.PhysicalPath;
				auto Copy = std::make_shared<T>(*Existing);
				Copy->Host = Existing->Host && !Host->IsEmpty() ? std::make_shared<const FContentBrowserOperationContext>(ComposeOperationContext(*Existing->Host, *Host)) : Existing->Host ? Existing->Host : Host;
				Payload = std::move(Copy); return true;
			};

				if (const auto File = std::dynamic_pointer_cast<const FContentBrowserFileItemDataPayload>(Item.GetPayload()))
				{
					Clone(File);
				}
				else Clone(std::dynamic_pointer_cast<const FContentBrowserFolderItemDataPayload>(Item.GetPayload()));

			if (!Payload && Item.GetItemType() == EContentBrowserItemFlags::Type_Folder)
			{
				auto Folder = std::make_shared<FContentBrowserFolderItemDataPayload>();
				Folder->Location.PhysicalPath = ContentStorage::FContentMountPaths::VirtualToPhysical(Item.GetVirtualPath()); Folder->Host = Host;
				Payload = std::move(Folder);
			}
			if (!Payload) return {};
			auto Virtual = Item.GetVirtualPath();
			if (Virtual.empty()) { const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Physical); if (Mount) Virtual = Mount.VirtualPath; }
			return {Source, Item.GetItemFlags() | EContentBrowserItemFlags::Category_Misc, std::move(Virtual), Item.GetItemName(), Item.GetDisplayName(), std::move(Payload)};
		}
		auto BindNative(DContentBrowserDataSource& Source, const FContentBrowserNativeItem& Native, std::shared_ptr<const FContentBrowserOperationContext> Host) -> FContentBrowserItemData
		{
			auto Resolved = Native; ResolveItemPath(Resolved);
			const auto Description = ContentBrowser::FileDataSource::Private::MakeNativeItemDescription(Resolved, std::move(Host));
			return {Source, Description.GetItemFlags(), Description.GetVirtualPath(), Description.GetItemName(), Description.GetDisplayName(), Description.GetPayload()};
		}
	} // namespace

	DContentBrowserFileDataSource::DContentBrowserFileDataSource(const FObjectInitializer& I)
		: Super(I)
	{
	}

	auto DContentBrowserFileDataSource::GetDataSourceName() const -> FName { return FName("File"); }

	auto DContentBrowserFileDataSource::BindItemData(const FContentBrowserItemData& Item, const FContentBrowserOperationContext& Host) -> FContentBrowserItemData
	{
		DContentBrowserDataSource& Source = *this;
		if (!Source.IsActive() || (Item.GetOwnerKey() != FObjectKey{} && Item.GetOwnerDataSource() != this)) return {};
		if (Host.IsEmpty() && Item.GetOwnerDataSource() == this) return Item;
		return Bind(Source, Item, std::make_shared<const FContentBrowserOperationContext>(Host));
	}

	auto DContentBrowserFileDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Filter, const std::function<bool(FContentBrowserItemData&&)>& Callback) -> void
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

	}

	auto DContentBrowserFileDataSource::GetItemAttribute(const FContentBrowserItemData& Data, bool IncludeMetaData, std::string_view Key, FContentBrowserItemDataAttributeValue& Out) const -> bool
	{
		using namespace ContentBrowser::FileDataSource::Private;
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

	auto DContentBrowserFileDataSource::GetItemAttributes(const FContentBrowserItemData& Data, bool IncludeMetaData, FContentBrowserItemDataAttributeValues& Out) const -> bool
	{
		using namespace ContentBrowserItemAttributes;
		return CollectAttributes(*this, Data, IncludeMetaData, {ItemTypeName, ItemTypeDisplayName, ItemDiskSize, Extension, LastWriteTime}, Out);
	}
	auto DContentBrowserFileDataSource::GetItemPhysicalPath(const FContentBrowserItemData& Data, std::string& Out) const -> bool
	{
		using namespace ContentBrowser::FileDataSource::Private;
		Out.clear();
		if (Data.GetOwnerDataSource()!=this) return false;
		if (const auto File=std::dynamic_pointer_cast<const FContentBrowserFileItemDataPayload>(Data.GetPayload())) Out=File->Location.PhysicalPath;
		else if (const auto Folder=std::dynamic_pointer_cast<const FContentBrowserFolderItemDataPayload>(Data.GetPayload())) Out=Folder->Location.PhysicalPath;
		return !Out.empty();
	}

	auto DContentBrowserFileDataSource::CanEditItem(const FContentBrowserItemData& Data, std::string*) const -> bool
	{
		auto P = Payload(*this, Data);
		return P && P->Item.Kind == EContentBrowserItemKind::File;
	}
	auto DContentBrowserFileDataSource::EditItem(const FContentBrowserItemData& Data) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		auto P = Payload(*this, Data);
		if (!P || !CanEditItem(Data)) return false;
#ifdef _WIN32
		const auto Path = std::filesystem::path(P->Item.PhysicalPath).make_preferred().wstring();
		return reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", Path.c_str(), nullptr, nullptr, SW_SHOW)) > 32;
#else
		return false;
#endif
	}

	auto DContentBrowserFileDataSource::CanCreateFolder(std::string_view Path, std::string* Error) const -> bool
	{
		const DContentBrowserDataSource& Source = *this;
		if (!Source.IsActive()) return false;
		const std::filesystem::path Virtual(Path);
		FFolderOperation Context(NativePolicy(ScopedHost(*const_cast<DContentBrowserDataSource*>(&Source), {})));
		return Context.CanCreateFolder(Destination(Virtual.parent_path().generic_string()), Virtual.filename().string(), Error);
	}

	auto DContentBrowserFileDataSource::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserOperationContext& Host) -> bool
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

	namespace
	{
		auto RenameTarget(const FContentBrowserNativeItem& Item, std::string_view Name) -> std::filesystem::path
		{
			auto Target = std::filesystem::path(Item.PhysicalPath).parent_path() / Name;
			if (Item.Kind == EContentBrowserItemKind::File && Target.extension().empty()) Target += std::filesystem::path(Item.PhysicalPath).extension();
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
		auto ExecuteNativeTransfer(DContentBrowserDataSource& Source, std::span<const FContentBrowserItemData> Data, std::string_view Dest, bool Move) -> bool
		{
			TStrongObjectPtr<DContentBrowserDataSource> Lifetime(&Source);
			const std::vector<FContentBrowserItemData> Snapshot(Data.begin(), Data.end());
			const std::string Target(Dest);
			std::vector<ContentBrowser::FileDataSource::Private::FNativeTransferInput> Inputs;
			for (const auto& Item : Snapshot)
			{
				const auto P = Payload(Source, Item); if (!P) continue;
				Inputs.push_back({P->Item, P->Host});
			}
			if (Inputs.empty()) return false;
			return ContentBrowser::FileDataSource::Private::ExecuteFileTransfer(std::move(Inputs), Destination(Target), Move);
		}
	}
	auto DContentBrowserFileDataSource::RenameItem(const FContentBrowserItemData& Input, std::string_view Name, FContentBrowserItemData& Out) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const auto Item = Input;
		const std::string NewName(Name);
		Out = {};
		const auto P = Payload(*this, Item); if (!P) return false;
		std::string Error;
		if (!ValidateRenameTarget(P->Item, NewName, &Error, true)) { if (P->Host->ReportDiagnostic) P->Host->ReportDiagnostic(ELogLevel::Error, Error); return false; }
		const auto Target = RenameTarget(P->Item, NewName);
		if (ContentBrowser::Transfer::Same(Target, P->Item.PhysicalPath)) { Out = Item; return true; }
		const std::vector<ContentBrowser::FileDataSource::Private::FNativeTransferInput> Inputs{{P->Item, P->Host}};
		const bool Success = ContentBrowser::FileDataSource::Private::ExecuteFileTransfer(Inputs, std::filesystem::path(P->Item.PhysicalPath).parent_path().generic_string(), true, NewName);
		if (!Success || Item.GetOwnerDataSource() != this) return Success;
		auto Renamed = P->Item; Renamed.Name = NewName; Renamed.PhysicalPath = Target.generic_string();
		Renamed.VirtualPath = ContentStorage::FContentMountPaths::ResolveMountPath(Renamed.PhysicalPath).VirtualPath;
		Out = BindNative(*this, Renamed, P->Context);
		return Success;
	}
	auto DContentBrowserFileDataSource::CopyItem(const FContentBrowserItemData& Item, std::string_view Destination) -> bool
	{ return BulkCopyItems(std::span{&Item, 1}, Destination); }
	auto DContentBrowserFileDataSource::BulkCopyItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> bool
	{ return ExecuteNativeTransfer(*this, Items, Destination, false); }
	auto DContentBrowserFileDataSource::MoveItem(const FContentBrowserItemData& Item, std::string_view Destination) -> bool
	{ return BulkMoveItems(std::span{&Item, 1}, Destination); }
	auto DContentBrowserFileDataSource::BulkMoveItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> bool
	{ return ExecuteNativeTransfer(*this, Items, Destination, true); }

	auto DContentBrowserFileDataSource::CanRenameItem(const FContentBrowserItemData& Item, std::optional<std::string_view> Name, std::string* Error) const -> bool
	{
		const auto CapturedItem = Item;
		const std::optional<std::string> CapturedName = Name ? std::optional(std::string(*Name)) : std::nullopt;
		const auto P = Payload(*this, CapturedItem);
		if (!P || !CanOperateSelectedFile(P->Item, Error) || CapturedItem.GetOwnerDataSource() != this) return false;
		const ContentBrowser::FileDataSource::Private::FNativeTransferInput Input{P->Item, P->Host};
		if (!ContentBrowser::Transfer::CanRename(Input, Error) || CapturedItem.GetOwnerDataSource() != this) return false;
		if (!CapturedName) return true;
		return ValidateRenameTarget(P->Item, *CapturedName, Error);
	}
	auto DContentBrowserFileDataSource::CanCopyItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		const std::string CapturedDestination(Dest);
		const auto CapturedItem = Item;
		const auto P = Payload(*this, CapturedItem);
		if (!P || !CanOperateSelectedFile(P->Item, Error) || CapturedItem.GetOwnerDataSource() != this) return false;
		const ContentBrowser::FileDataSource::Private::FNativeTransferInput Input{P->Item, P->Host};
		return ContentBrowser::Transfer::CanTransfer(Input, Destination(CapturedDestination), false, Error) && CapturedItem.GetOwnerDataSource() == this;
	}
	auto DContentBrowserFileDataSource::CanMoveItem(const FContentBrowserItemData& Item, std::string_view Dest, std::string* Error) const -> bool
	{
		const std::string CapturedDestination(Dest);
		const auto CapturedItem = Item;
		const auto P = Payload(*this, CapturedItem);
		if (!P || !CanOperateSelectedFile(P->Item, Error) || CapturedItem.GetOwnerDataSource() != this) return false;
		const ContentBrowser::FileDataSource::Private::FNativeTransferInput Input{P->Item, P->Host};
		return ContentBrowser::Transfer::CanTransfer(Input, Destination(CapturedDestination), true, Error) && CapturedItem.GetOwnerDataSource() == this;
	}

	auto DContentBrowserFileDataSource::CanDeleteItem(const FContentBrowserItemData& Item, std::string* Error) const -> bool
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

	auto DContentBrowserFileDataSource::DeleteItem(const FContentBrowserItemData& Item) -> bool
	{ return BulkDeleteItems(std::span(&Item, 1)); }

	auto DContentBrowserFileDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData> Data) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Data.begin(), Data.end());
		std::vector<FContentBrowserNativeItem> Items;
		auto Operations = GatherDeletion(*this, Snapshot, Items);
		if (!Operations) return false;
		std::vector<ContentStorage::FFileDeletionInput> Inputs;
		for (const auto& Item : Items) Inputs.push_back({Item.Name, Item.PhysicalPath});
		Operations->DeleteWithConfirmation(Inputs);
		return Operations->HasCommittedEffects();
	}
} // namespace Durin

namespace Durin::ContentBrowser::FileDataSource::Private
{
	auto QueueNativeCommittedUpdates(DContentBrowserDataSource& Source, const FContentChangeBatch& Batch, bool FromDiscovery) -> void
	{
		RecordContentBrowserFileUpdates(static_cast<DContentBrowserFileDataSource&>(Source), Batch, FromDiscovery);
		QueueCommittedUpdates(Source, Batch, FromDiscovery);
	}
}
