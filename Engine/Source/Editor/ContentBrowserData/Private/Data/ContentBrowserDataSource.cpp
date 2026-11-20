#include "ContentBrowserDataSource.h"
#include "Threading/RunnableThread.h"
#include "Editor/EditorEngine.h"
namespace Durin
{
	using namespace ContentBrowser;
	auto DContentBrowserDataSource::OnDataSourceMembershipChanged() -> TMulticastDelegate<void()>& { static TMulticastDelegate<void()> Event; return Event; }
	DContentBrowserDataSource::DContentBrowserDataSource(const FObjectInitializer& I)
		: Super(I)
	{
	}
	auto DContentBrowserDataSource::GetOwningEditor() const -> DEditorEngine*
	{
		for (auto* Outer = GetOuter(); Outer; Outer = Outer->GetOuter())
			if (auto* Editor = Cast<DEditorEngine>(Outer)) return Editor;
		return nullptr;
	}
	auto DContentBrowserDataSource::IsActive() const -> bool
	{
		if (!IsInGameThread() || !bInitialized || !Registration.IsValid() || !DataSink) return false;
		const auto* Editor = GetOwningEditor();
		return !Editor || !Editor->IsShutdownRequested();
	}
	auto DContentBrowserDataSource::Initialize(bool AutoRegister) -> void
	{
		CheckGameThread();
		bInitialized = true;
		if (AutoRegister) RegisterDataSource();
	}
	auto DContentBrowserDataSource::RegisterDataSource() -> void
	{
		CheckGameThread();
		if (bInitialized && !Registration.IsValid())
		{
			FStrongObjectPtr KeepAlive(this);
			Registration = RegisterModularFeature<DContentBrowserDataSource>(*this);
			RegistrationRoot = std::move(KeepAlive);
			ClearCachedFilterData();
			OnDataSourceMembershipChanged().Broadcast();
		}
	}
	auto DContentBrowserDataSource::UnregisterDataSource() -> void
	{
		CheckGameThread();
		requiref(!IsModularFeatureDispatching(), "Data source withdrawal requires a modular-feature safe point.");
		FStrongObjectPtr KeepAlive(Registration.IsValid() ? this : nullptr);
		const bool Registered = Registration.IsValid();
		SetDataSink(nullptr);
		Registration.Reset();
		RegistrationRoot.Reset();
		if (Registered) OnDataSourceMembershipChanged().Broadcast();
	}
	auto DContentBrowserDataSource::Shutdown() -> void
	{
		UnregisterDataSource();
		bInitialized = false;
	}
	auto DContentBrowserDataSource::BeginDestroy() -> void
	{
		requiref(!Registration.IsValid(), "Registered data sources must retire at an owner safe point before collection.");
		Shutdown();
		Super::BeginDestroy();
	}
	auto DContentBrowserDataSource::SetDataSink(IContentBrowserItemDataSink* Sink) -> void
	{
		CheckGameThread();
		if (Sink != DataSink)
		{
			DataSink = Sink;
			ClearCachedFilterData();
		}
	}
	auto DContentBrowserDataSource::GetDataSourceName() const -> FName { return GetFName(); }
	auto DContentBrowserDataSource::QueueItemDataUpdate(FContentBrowserItemDataUpdate&& Update) -> void
	{
		if (IsActive() && Update.GetItemData().GetOwnerDataSource() == this) { ClearCachedFilterData(); DataSink->QueueItemDataUpdate(std::move(Update)); }
	}
	auto DContentBrowserDataSource::NotifyItemDataRefreshed() -> void
	{
		ClearCachedFilterData();
		if (IsActive()) DataSink->NotifyItemDataRefreshed();
	}
	auto DContentBrowserDataSource::CompileFilter(std::string_view Path, const FContentBrowserDataFilter& Filter, FContentBrowserDataCompiledFilter& Out) const -> void { Out.Path = std::string(Path); Out.Filter = Filter; Out.CompiledFilters[this] = {}; }
	auto DContentBrowserDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter&, const std::function<bool(FContentBrowserItemData&&)>&) -> void {}
	auto DContentBrowserDataSource::DoesItemPassFilter(const FContentBrowserItemData& Item, const FContentBrowserDataCompiledFilter& Filter) const -> bool
	{
		if (!IsActive() || Item.GetOwnerDataSource() != this || !Filter.CompiledFilters.contains(this)) return false;
		const auto Scope = FContentBrowserItemDataKey(EContentBrowserItemFlags::Type_Folder, Filter.Path).GetVirtualPath();
		const auto Path = Item.GetItemDataKey().GetVirtualPath();
		if (Scope.empty() || Path == Scope || !Path.starts_with(Scope == "/" ? Scope : Scope + "/")) return false;
		if (!Filter.Filter.bRecursive && std::filesystem::path(Path).parent_path().generic_string() != Scope) return false;
		if (!Filter.Filter.Kind) return true;
		const bool Folder = Item.GetItemType() == EContentBrowserItemFlags::Type_Folder;
		const bool Asset = (static_cast<uint16>(Item.GetItemFlags()) & static_cast<uint16>(EContentBrowserItemFlags::Category_Asset)) != 0;
		FContentBrowserItemDataAttributeValue Redirector;
		const bool IsRedirector = !Folder && Asset && Item.GetItemAttribute(ContentBrowserItemAttributes::IsRedirector, Redirector)
			&& Redirector.GetValue<bool>() && *Redirector.GetValue<bool>();
		const auto Kind = Folder ? EContentBrowserItemKind::Folder : Asset
			? (IsRedirector ? EContentBrowserItemKind::Redirector : EContentBrowserItemKind::Asset) : EContentBrowserItemKind::File;
		return Kind == *Filter.Filter.Kind;
	}
	auto DContentBrowserDataSource::BindItemData(const FContentBrowserItemData& Item, const FContentBrowserOperationContext& Host) -> FContentBrowserItemData
	{
		if (!IsActive() || (Item.GetOwnerKey() != FObjectKey{} && Item.GetOwnerDataSource() != this)) return {};
		if (Host.IsEmpty() && Item.GetOwnerDataSource() == this) return Item;
		return {*this, Item.GetItemFlags(), Item.GetVirtualPath(), Item.GetItemName(), Item.GetDisplayName(), Item.GetPayload()};
	}
	auto DContentBrowserDataSource::EnumerateItemsAtPath(std::string_view Path, const std::function<bool(FContentBrowserItemData&&)>& Callback) -> void
	{
		std::string Target(Path);
		while (Target.size() > 1 && Target.back() == '/')
			Target.pop_back();
		const auto Slash = Target.find_last_of('/');
		if (Slash == std::string::npos) return;
		FContentBrowserDataCompiledFilter Compiled;
		CompileFilter(Target.substr(0, Slash + 1), {}, Compiled);
		EnumerateItemsMatchingFilter(Compiled, [&](FContentBrowserItemData&& Item) {
   std::string Candidate = Item.GetVirtualPath(); while (Candidate.size() > 1 && Candidate.back() == '/') Candidate.pop_back();
   return Candidate != Target || Callback(std::move(Item)); });
	}

	auto DContentBrowserDataSource::GetItemAttribute(const FContentBrowserItemData&, bool, std::string_view,
													 FContentBrowserItemDataAttributeValue& Out) const -> bool
	{
		Out = {};
		return false;
	}
	auto DContentBrowserDataSource::GetItemAttributes(const FContentBrowserItemData&, bool,
													  FContentBrowserItemDataAttributeValues& Out) const -> bool
	{
		Out.clear();
		return false;
	}
	auto DContentBrowserDataSource::GetItemPhysicalPath(const FContentBrowserItemData&, std::string& Out) const -> bool
	{
		Out.clear();
		return false;
	}

	auto DContentBrowserDataSource::CanEditItem(const FContentBrowserItemData&, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::EditItem(const FContentBrowserItemData&) -> bool { return false; }
	auto DContentBrowserDataSource::CanDuplicateItem(const FContentBrowserItemData&, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::DuplicateItem(const FContentBrowserItemData&, FContentBrowserItemData&) -> bool { return false; }
	auto DContentBrowserDataSource::CanCreateFolder(std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::CreateFolder(std::string_view, FContentBrowserItemDataTemporaryContext&, const FContentBrowserOperationContext&) -> bool { return false; }

	auto DContentBrowserDataSource::CanSaveItem(const FContentBrowserItemData&, EContentBrowserItemSaveFlags, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::BulkSaveItems(std::span<const FContentBrowserItemData>, EContentBrowserItemSaveFlags) -> bool { return false; }
	auto DContentBrowserDataSource::CanRenameItem(const FContentBrowserItemData&, std::optional<std::string_view>, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::RenameItem(const FContentBrowserItemData&, std::string_view, FContentBrowserItemData& Out) -> bool { Out = {}; return false; }
	auto DContentBrowserDataSource::CanCopyItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::CopyItem(const FContentBrowserItemData&, std::string_view) -> bool { return false; }
	auto DContentBrowserDataSource::BulkCopyItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		const std::string Target(Destination);
		bool Success = false;
		for (const auto& Item : Snapshot) if (Item.GetOwnerDataSource() == this) Success |= CopyItem(Item, Target);
		return Success;
	}
	auto DContentBrowserDataSource::CanMoveItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::MoveItem(const FContentBrowserItemData&, std::string_view) -> bool { return false; }
	auto DContentBrowserDataSource::BulkMoveItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		const std::string Target(Destination);
		bool Success = false;
		for (const auto& Item : Snapshot) if (Item.GetOwnerDataSource() == this) Success |= MoveItem(Item, Target);
		return Success;
	}
	auto DContentBrowserDataSource::CanDeleteItem(const FContentBrowserItemData&, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::DeleteItem(const FContentBrowserItemData&) -> bool { return false; }
	auto DContentBrowserDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData> Items) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		bool Success = false;
		for (const auto& Item : Snapshot)
			if (IsActive() && Item.GetOwnerDataSource() == this) Success |= DeleteItem(Item);
		return Success;
	}

} // namespace Durin
namespace Durin::ContentBrowser
{
	FContentBrowserItemData::FContentBrowserItemData() = default;
	FContentBrowserItemData::FContentBrowserItemData(EContentBrowserItemFlags InFlags, std::string Path, std::string InName, std::string InDisplayName, std::shared_ptr<const IContentBrowserItemDataPayload> P)
		: Flags(InFlags)
		, VirtualPath(std::move(Path))
		, Name(std::move(InName))
		, DisplayName(std::move(InDisplayName))
		, Payload(std::move(P))
	{
		if (Name.empty())
		{
			auto LeafPath=VirtualPath;
			while (LeafPath.size()>1 && LeafPath.back()=='/') LeafPath.pop_back();
			Name=std::filesystem::path(LeafPath).filename().generic_string();
		}
		if (DisplayName.empty()) DisplayName=Name;
	}
	FContentBrowserItemData::FContentBrowserItemData(DContentBrowserDataSource& Source, EContentBrowserItemFlags InFlags, std::string Path, std::string InName, std::string InDisplayName, std::shared_ptr<const IContentBrowserItemDataPayload> P)
		: FContentBrowserItemData(InFlags, std::move(Path), std::move(InName), std::move(InDisplayName), std::move(P))
	{
		CheckGameThread();
		Owner=&Source;
	}

	auto FContentBrowserItemData::GetOwnerDataSource() const -> DContentBrowserDataSource*
	{
		if (!IsInGameThread()) return nullptr;
		return Owner.Get();
	}
	auto FContentBrowserItemData::GetItemAttribute(std::string_view Key, FContentBrowserItemDataAttributeValue& Out, bool IncludeMetaData) const -> bool
	{
		Out={};
		auto* Source=GetOwnerDataSource();
		if (!Source) return false;
		TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
		if (Source->GetItemAttribute(*this, IncludeMetaData, Key, Out) && Out.IsValid() && GetOwnerDataSource()==Source) return true;
		Out={}; return false;
	}
	auto FContentBrowserItemData::GetItemAttributes(FContentBrowserItemDataAttributeValues& Out, bool IncludeMetaData) const -> bool
	{
		Out.clear();
		auto* Source=GetOwnerDataSource();
		if (!Source) return false;
		TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
		if (Source->GetItemAttributes(*this, IncludeMetaData, Out) && GetOwnerDataSource()==Source) return true;
		Out.clear(); return false;
	}
	auto FContentBrowserItemData::GetItemPhysicalPath(std::string& Out) const -> bool
	{
		Out.clear();
		auto* Source=GetOwnerDataSource();
		if (!Source) return false;
		TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
		if (Source->GetItemPhysicalPath(*this, Out) && !Out.empty() && GetOwnerDataSource()==Source) return true;
		Out.clear(); return false;
	}

} // namespace Durin::ContentBrowser
