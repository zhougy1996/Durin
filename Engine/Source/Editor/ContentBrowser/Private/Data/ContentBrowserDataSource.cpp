#include "ContentBrowserDataSource.h"
#include "Threading/RunnableThread.h"
#include "Editor/EditorEngine.h"
#include "Misc/MountPaths.h"
namespace Durin
{
	using namespace ContentBrowser;
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
		}
	}
	auto DContentBrowserDataSource::UnregisterDataSource() -> void
	{
		CheckGameThread();
		requiref(!FModularFeatureRegistry::Get().IsDispatching(), "Data source withdrawal requires a modular-feature safe point.");
		SetDataSink(nullptr);
		Registration.Reset();
		RegistrationRoot.Reset();
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
	auto DContentBrowserDataSource::CompileFilter(std::string_view Path, const FContentBrowserDataFilter& Filter, FContentBrowserDataCompiledFilter& Out) const -> void { Out = {std::string(Path), Filter}; }
	auto DContentBrowserDataSource::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter&, const std::function<bool(FContentBrowserItem&&)>&, const FContentBrowserHostContext&) -> void {}
	auto DContentBrowserDataSource::CreateItemData(const FContentBrowserItem&, const FContentBrowserHostContext&) -> FContentBrowserItemData { return {}; }
	auto DContentBrowserDataSource::EnumerateItemsAtPath(std::string_view Path, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserHostContext& Host) -> void
	{
		std::string Target(Path);
		while (Target.size() > 1 && Target.back() == '/')
			Target.pop_back();
		const auto Slash = Target.find_last_of('/');
		if (Slash == std::string::npos) return;
		FContentBrowserDataCompiledFilter Compiled;
		CompileFilter(Target.substr(0, Slash + 1), {}, Compiled);
		EnumerateItemsMatchingFilter(Compiled, [&](FContentBrowserItem&& Item) {
   std::string Candidate = Item.VirtualPath; while (Candidate.size() > 1 && Candidate.back() == '/') Candidate.pop_back();
   return Candidate != Target || Callback(std::move(Item)); }, Host);
	}
	auto DContentBrowserDataSource::CanEditItem(const FContentBrowserItemData&, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::EditItem(const FContentBrowserItemData&) -> bool { return false; }
	auto DContentBrowserDataSource::CanDuplicateItem(const FContentBrowserItemData&, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::DuplicateItem(const FContentBrowserItemData&, FContentBrowserItem&) -> bool { return false; }
	auto DContentBrowserDataSource::CanCreateFolder(std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::CreateFolder(std::string_view, FContentBrowserItemDataTemporaryContext&, const FContentBrowserHostContext&) -> bool { return false; }

	auto DContentBrowserDataSource::CanSaveItem(const FContentBrowserItemData&, EContentBrowserItemSaveFlags, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::BulkSaveItems(std::span<const FContentBrowserItemData>, EContentBrowserItemSaveFlags) -> bool { return false; }
	auto DContentBrowserDataSource::CanRenameItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::RenameItem(const FContentBrowserItemData&, std::string_view, FContentBrowserItem&) -> bool { return false; }
	auto DContentBrowserDataSource::CanCopyItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::BulkCopyItems(std::span<const FContentBrowserItemData>, std::string_view) -> bool { return false; }
	auto DContentBrowserDataSource::CanMoveItem(const FContentBrowserItemData&, std::string_view, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::BulkMoveItems(std::span<const FContentBrowserItemData>, std::string_view) -> bool { return false; }
	auto DContentBrowserDataSource::CanDeleteItem(const FContentBrowserItemData&, std::string*) const -> bool { return false; }
	auto DContentBrowserDataSource::PrepareDeleteItems(std::span<const FContentBrowserItemData>) -> FContentBrowserDeleteContext { return {}; }
	auto DContentBrowserDataSource::BulkDeleteItems(std::span<const FContentBrowserItemData>, FContentBrowserDeleteContext&) -> bool { return false; }
} // namespace Durin
namespace Durin::ContentBrowser
{
	FContentBrowserItemData::FContentBrowserItemData() = default;
	FContentBrowserItemData::FContentBrowserItemData(DContentBrowserDataSource& Source, EContentBrowserItemKind K, std::string Path, std::shared_ptr<const IContentBrowserItemDataPayload> P)
		: Kind(K)
		, VirtualPath(std::move(Path))
		, Payload(std::move(P))
	{
		CheckGameThread();
		if (Source.IsActive())
		{
			Owner = &Source;
			Generation = Source.GetActivationGeneration();
			MountRevision = FMountPaths::GetMountRegistryRevision();
		}
	}
	auto FContentBrowserItemData::GetOwnerDataSource() const -> DContentBrowserDataSource*
	{
		if (!IsInGameThread() || MountRevision != FMountPaths::GetMountRegistryRevision()) return nullptr;
		auto* Source = Owner.Get();
		return Source && Source->IsActive() && Source->GetActivationGeneration() == Generation ? Source : nullptr;
	}
} // namespace Durin::ContentBrowser
