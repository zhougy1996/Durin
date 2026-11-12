#include "ContentBrowserDataSource.h"
#include "Threading/RunnableThread.h"
#include "Editor/EditorEngine.h"
#include "Misc/MountPaths.h"
namespace Durin
{
	using namespace ContentBrowser;
	auto DContentBrowserDataSource::OnDataSourceMembershipChanged() -> TMulticastDelegate<void()>& { static TMulticastDelegate<void()> Event; return Event; }
	DContentBrowserDataSource::DContentBrowserDataSource(const FObjectInitializer& I)
		: Super(I)
	{
	}
	auto DContentBrowserDataSource::IsActive() const -> bool
	{
		if (!IsInGameThread() || !bInitialized || !Registration.IsValid() || !DataSink) return false;
		for (auto* Outer = GetOuter(); Outer; Outer = Outer->GetOuter())
			if (auto* Editor = Cast<DEditorEngine>(Outer)) return !Editor->IsShutdownRequested();
		return true;
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
			Registration = FModularFeatureRegistry::Get().RegisterFeature<DContentBrowserDataSource>(*this);
			RegistrationRoot = std::move(KeepAlive);
			++Generation;
			OnDataSourceMembershipChanged().Broadcast();
		}
	}
	auto DContentBrowserDataSource::UnregisterDataSource() -> void
	{
		CheckGameThread();
		requiref(!FModularFeatureRegistry::Get().IsDispatching(), "Data source withdrawal requires a modular-feature safe point.");
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
			++Generation;
			DataSink = Sink;
		}
	}
	auto DContentBrowserDataSource::GetDataSourceName() const -> FName { return GetFName(); }
	auto DContentBrowserDataSource::QueueItemDataUpdate(FContentBrowserItemDataUpdate&& Update) -> void
	{
		if (IsActive() && Update.GetItemData().GetOwnerDataSource() == this) DataSink->QueueItemDataUpdate(std::move(Update));
	}
	auto DContentBrowserDataSource::CompileFilter(std::string_view Path, const FContentBrowserDataFilter& Filter, FContentBrowserDataCompiledFilter& Out) const -> void { Out.Path = std::string(Path); Out.Filter = Filter; Out.CompiledFilters[this] = {GetActivationGeneration(), FMountPaths::GetMountRegistryRevision(), GetDiscoveryRevision(), {}}; }
	auto DContentBrowserDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter&, const std::function<bool(FContentBrowserItem&&)>&, const FContentBrowserOperationContext&) -> void {}
	auto DContentBrowserDataSource::CreateItemData(const FContentBrowserItem&, const FContentBrowserOperationContext&) -> FContentBrowserItemData { return {}; }
	auto DContentBrowserDataSource::EnumerateItemsAtPath(std::string_view Path, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserOperationContext& Host) -> void
	{
		std::string Target(Path);
		while (Target.size() > 1 && Target.back() == '/')
			Target.pop_back();
		const auto Slash = Target.find_last_of('/');
		if (Slash == std::string::npos) return;
		FContentBrowserDataCompiledFilter Compiled;
		CompileFilter(Target.substr(0, Slash + 1), {}, Compiled);
		EnumerateItemsMatchingFilter(Compiled, [&](FContentBrowserItem&& Item) {
   std::string Candidate = Item.GetVirtualPath(); while (Candidate.size() > 1 && Candidate.back() == '/') Candidate.pop_back();
   return Candidate != Target || Callback(std::move(Item)); }, Host);
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
	auto DContentBrowserDataSource::BulkSaveItems(std::span<const FContentBrowserItemData> Items, EContentBrowserItemSaveFlags) -> ContentBrowser::FContentBrowserOperationResult
	{
		FContentBrowserOperationResult Result;
		for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Failed, "This source does not support saving.");
		return Result;
	}
	auto DContentBrowserDataSource::CanRenameItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::PrepareRenameItem(const FContentBrowserItemData&, std::string_view) -> std::unique_ptr<FContentBrowserItemDataTransferContext> { return {}; }
	auto DContentBrowserDataSource::RenameItem(const FContentBrowserItemData& Item, std::string_view Name, FContentBrowserItemData& Out) -> FContentBrowserOperationResult
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const auto Captured = Item;
		auto Context = PrepareRenameItem(Captured, Name);
		Out = {};
		if (!Context) { FContentBrowserOperationResult Result; Result.Add(Captured.GetContributionKey(), EContentBrowserEffectStatus::Failed, "The source does not support renaming."); return Result; }
		const auto Result = Context->Execute();
		Out = Context->GetResultItemData();
		return Result;
	}

	auto DContentBrowserDataSource::CanCopyItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::PrepareCopyItems(std::span<const FContentBrowserItemData>, std::string_view) -> std::unique_ptr<FContentBrowserItemDataTransferContext> { return {}; }
	auto DContentBrowserDataSource::BulkCopyItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> FContentBrowserOperationResult
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		Items = Snapshot;
		auto Context = PrepareCopyItems(Items, Destination);
		if (Context) return Context->Execute();
		FContentBrowserOperationResult Result;
		for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Failed, "The source does not support this transfer.");
		return Result;
	}
	auto DContentBrowserDataSource::CanMoveItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::PrepareMoveItems(std::span<const FContentBrowserItemData>, std::string_view) -> std::unique_ptr<FContentBrowserItemDataTransferContext> { return {}; }
	auto DContentBrowserDataSource::BulkMoveItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination) -> FContentBrowserOperationResult
	{
		TStrongObjectPtr<DContentBrowserDataSource> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		Items = Snapshot;
		auto Context = PrepareMoveItems(Items, Destination);
		if (Context) return Context->Execute();
		FContentBrowserOperationResult Result;
		for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Failed, "The source does not support this transfer.");
		return Result;
	}
	auto DContentBrowserDataSource::CanDeleteItem(const FContentBrowserItemData&, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::PrepareDeleteItems(std::span<const FContentBrowserItemData>) -> FContentBrowserDeleteContext { return {}; }
	auto DContentBrowserDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData>, FContentBrowserDeleteContext&) -> ContentBrowser::FContentBrowserOperationResult { return {}; }
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
		if (Source.IsActive())
		{
			Owner=&Source; Generation=Source.GetActivationGeneration(); MountRevision=FMountPaths::GetMountRegistryRevision();
		}
	}

	auto FContentBrowserItemData::GetOwnerDataSource() const -> DContentBrowserDataSource*
	{
		if (!IsInGameThread() || MountRevision != FMountPaths::GetMountRegistryRevision()) return nullptr;
		auto* Source = Owner.Get();
		return Source && Source->IsActive() && Source->GetActivationGeneration() == Generation ? Source : nullptr;
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
