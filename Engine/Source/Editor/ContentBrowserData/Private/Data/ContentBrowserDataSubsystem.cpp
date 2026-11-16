#include "ContentBrowserDataSubsystem.h"
#include "Data/ContentBrowserSaveDispatch.h"
#include "Data/ContentBrowserTransferDispatch.h"
#include "Data/ContentBrowserDeleteDispatch.h"
#include "Editor/EditorEngine.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/MountPaths.h"
namespace Durin
{
	using namespace ContentBrowser;
	DContentBrowserDataSubsystem::DContentBrowserDataSubsystem(const FObjectInitializer& I)
		: Super(I)
	{
	}
	auto DContentBrowserDataSubsystem::Initialize(FSubsystemCollection&) -> void
	{
		bStopping = false;
		SourceMembershipChanged = DContentBrowserDataSource::OnDataSourceMembershipChanged().AddLambda([this] { RefreshSources(); });
		MountsChanged = FMountPaths::OnMountPointsChanged().AddLambda([Weak = TWeakObjectPtr<DContentBrowserDataSubsystem>(this)] {
			if (auto* Data = Weak.Get()) Data->NotifyItemDataRefreshed();
		});
		RefreshSources();
	}
	auto DContentBrowserDataSubsystem::Deinitialize() noexcept -> void
	{
		bStopping = true;
		FMountPaths::OnMountPointsChanged().Remove(MountsChanged); MountsChanged = {};
		DContentBrowserDataSource::OnDataSourceMembershipChanged().Remove(SourceMembershipChanged);
		for (auto& Source : Active)
			Source->SetDataSink(nullptr);
		Active.clear();
		Available.clear();
		PendingUpdates.clear();
		ItemDataUpdated.Clear();
		ItemDataRefreshed.Clear();
		NotifyItemDataRefreshed();
	}
	auto DContentBrowserDataSubsystem::RefreshSources() -> void
	{
		if (bStopping) return;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Next;
		FModularFeatureRegistry::Get().InvokeAll<DContentBrowserDataSource>([&](DContentBrowserDataSource& Source) {
			if (Source.GetOuter() == this || Source.GetOuter() == GetEditor()) Next.emplace_back(&Source);
		});
		if (Available.size() != Next.size() || !std::ranges::equal(Available, Next, [](const auto& A, const auto& B) { return A.Get() == B.Get(); })) NotifyItemDataRefreshed();
		Available = std::move(Next);
		std::erase_if(Active, [&](const auto& Source) {
			if (!Source->IsActive() || !std::ranges::any_of(Available, [&](const auto& S) { return S.Get() == Source.Get(); }))
			{
				Source->SetDataSink(nullptr);
				NotifyItemDataRefreshed();
				return true;
			}
			return false;
		});
	}
	auto DContentBrowserDataSubsystem::ActivateDataSource(FName Name) -> bool
	{
		if (bStopping) return false;
		RefreshSources();
		DContentBrowserDataSource* Found = nullptr;
		for (const auto& Source : Available)
			if (Source->GetDataSourceName() == Name)
			{
				if (Found) return false;
				Found = Source.Get();
			}
		if (!Found || !Found->IsInitialized()) return false;
		if (GetActiveDataSource(Name) == Found) return true;
		Found->SetDataSink(this);
		Active.emplace_back(Found);
		NotifyItemDataRefreshed();
		return true;
	}
	auto DContentBrowserDataSubsystem::DeactivateDataSource(FName Name) -> bool
	{
		auto* Source = GetActiveDataSource(Name);
		if (!Source) return false;
		Source->SetDataSink(nullptr);
		std::erase_if(Active, [Source](const auto& S) { return S.Get() == Source; });
		NotifyItemDataRefreshed();
		return true;
	}
	auto DContentBrowserDataSubsystem::GetActiveDataSource(FName Name) const -> DContentBrowserDataSource*
	{
		if (bStopping) return nullptr;
		for (const auto& Source : Active)
			if (Source->IsActive() && Source->GetDataSourceName() == Name) return Source.Get();
		return nullptr;
	}
	auto DContentBrowserDataSubsystem::OwnsSource(const DContentBrowserDataSource* Source) const -> bool
	{
		return !bStopping && GetEditor() && !GetEditor()->IsShutdownRequested() && Source && Source->IsActive()
			   && std::ranges::any_of(Active, [Source](const auto& S) { return S.Get() == Source; });
	}
	auto DContentBrowserDataSubsystem::Tick(float DeltaSeconds) noexcept -> void
	{
		if (!IsActive()) return;
		TStrongObjectPtr<DContentBrowserDataSubsystem> Lifetime(this);
		RefreshSources();
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get())) Source->Tick(DeltaSeconds);
		if (!IsActive()) return;
		for (size_t I = PendingUpdates.size(); I-- > 0;)
			if (!OwnsSource(PendingUpdates[I].GetItemData().GetOwnerDataSource()))
			{
				PendingUpdates.erase(PendingUpdates.begin() + I);
			}
		const auto Updates = std::move(PendingUpdates);
		PendingUpdates.clear();
		const bool Refreshed = std::exchange(bPendingItemDataRefreshed, false);
		if (Refreshed) ItemDataRefreshed.Broadcast();
		if (IsActive() && !Updates.empty()) ItemDataUpdated.Broadcast(Updates);
	}
	auto DContentBrowserDataSubsystem::NotifyItemDataRefreshed() -> void
	{
		if (!bStopping) bPendingItemDataRefreshed = true;
	}
	auto DContentBrowserDataSubsystem::QueueItemDataUpdate(FContentBrowserItemDataUpdate&& Update) -> void
	{
		if (!OwnsSource(Update.GetItemData().GetOwnerDataSource())) return;
		PendingUpdates.push_back(std::move(Update));
	}
	auto DContentBrowserDataSubsystem::MergeItem(std::vector<FContentBrowserItem>& Items, FContentBrowserItem Item) -> void
	{
		auto Folder = Item.IsFolder()
						  ? std::ranges::find_if(Items,
												 [&](const auto& Existing)
												 {
													 return Item.GetItemKey().IsValid() && Existing.GetItemKey() == Item.GetItemKey();
												 })
						  : Items.end();
		if (Folder == Items.end())
			Items.push_back(std::move(Item));
		else
		{
			for (auto& Data : Item.InternalItems)
				if (std::ranges::none_of(Folder->InternalItems, [&](const auto& Existing) { return Existing.GetContributionKey() == Data.GetContributionKey(); }))
					Folder->InternalItems.push_back(std::move(Data));
		}
	}

	auto DContentBrowserDataSubsystem::CompileFilter(std::string_view Path, const FContentBrowserDataFilter& Filter,
													 FContentBrowserDataCompiledFilter& Out) -> void
	{
		RefreshSources();
		Out = {std::string(Path), Filter};
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get()))
				Source->CompileFilter(Path, Filter, Out);
	}
	auto DContentBrowserDataSubsystem::EnumerateItemsUnderPath(std::string_view Path, const FContentBrowserDataFilter& Filter,
															   const std::function<bool(FContentBrowserItem&&)>& Callback,
															   const FContentBrowserOperationContext& Host) -> void
	{
		FContentBrowserDataCompiledFilter Compiled;
		CompileFilter(Path, Filter, Compiled);
		EnumerateItemsMatchingFilter(Compiled, Callback, Host);
	}
	auto DContentBrowserDataSubsystem::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Compiled,
																	const std::function<bool(FContentBrowserItem&&)>& Callback,
																	const FContentBrowserOperationContext& Host) -> void

	{
		RefreshSources();
		const auto Sources = Active;
		std::vector<FContentBrowserItem> Items;
		for (const auto& Source : Sources)
		{
			if (!OwnsSource(Source.Get()))
				continue;
			const auto Entry = Compiled.CompiledFilters.find(Source.Get());
			if (Entry == Compiled.CompiledFilters.end())
				continue;
			Source->EnumerateItemsMatchingFilter(
				Compiled,
				[&](FContentBrowserItemData&& Item)
				{
					if (!OwnsSource(Source.Get()))
						return false;
					if (Item.GetOwnerDataSource() != Source.Get()) return true;
					auto Data = Source->BindItemData(Item, Host);
					if (Data.GetOwnerDataSource() == Source.Get() && OwnsSource(Source.Get()))
						MergeItem(Items, FContentBrowserItem{.InternalItems = {std::move(Data)}});
					return true;
				});
		}
		for (auto& Item : Items)
		{
			std::erase_if(Item.InternalItems, [&](const auto& Data) { return !OwnsSource(Data.GetOwnerDataSource()); });
			if (!Item.InternalItems.empty() && !Callback(std::move(Item)))
				return;
		}
	}
	auto DContentBrowserDataSubsystem::EnumerateItemsAtPath(std::string_view Path, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserOperationContext& Host) -> void
	{
		std::string Target(Path);
		while (Target.size() > 1 && Target.back() == '/')
			Target.pop_back();
		const auto Slash = Target.find_last_of('/');
		if (Slash == std::string::npos) return;
		EnumerateItemsUnderPath(Target.substr(0, Slash + 1), {}, [&](FContentBrowserItem&& Item) {
   std::string Candidate = Item.GetVirtualPath(); while (Candidate.size() > 1 && Candidate.back() == '/') Candidate.pop_back();
   return Candidate != Target || Callback(std::move(Item)); }, Host);
	}
	auto DContentBrowserDataSubsystem::BulkSaveItems(std::span<const FContentBrowserItemData> Items, EContentBrowserItemSaveFlags Flags) -> bool
	{
		RefreshSources();
		return ContentBrowser::Private::DispatchSaveItems(Items, Flags, [this](auto* Source) { return OwnsSource(Source); });
	}

	auto DContentBrowserDataSubsystem::IsActive() const -> bool
	{
		return !bStopping && GetEditor() && !GetEditor()->IsShutdownRequested();
	}
	auto DContentBrowserDataSubsystem::CanCreateFolder(std::string_view Path, std::string* Error) -> bool
	{
		RefreshSources();
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get()) && Source->CanCreateFolder(Path, Error)) return true;
		return false;
	}
	auto DContentBrowserDataSubsystem::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserOperationContext& Host) -> bool
	{
		RefreshSources();
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get()) && Source->CanCreateFolder(Path) && OwnsSource(Source.Get())) return Source->CreateFolder(Path, Out, Host);
		return false;
	}


	auto DContentBrowserDataSubsystem::BulkCopyItems(std::span<const FContentBrowserItemData> Items, std::string_view Dest) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSubsystem> Lifetime(this);
		RefreshSources();
		return ContentBrowser::Private::DispatchTransferItems(Items, Dest, false, [this](auto* Source) { return OwnsSource(Source); });
	}
	auto DContentBrowserDataSubsystem::BulkMoveItems(std::span<const FContentBrowserItemData> Items, std::string_view Dest) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSubsystem> Lifetime(this);
		RefreshSources();
		return ContentBrowser::Private::DispatchTransferItems(Items, Dest, true, [this](auto* Source) { return OwnsSource(Source); });
	}
	auto DContentBrowserDataSubsystem::BulkDeleteItems(std::span<const FContentBrowserItemData> Items) -> bool
	{
		TStrongObjectPtr<DContentBrowserDataSubsystem> Lifetime(this);
		RefreshSources();
		return ContentBrowser::Private::DispatchDeleteItems(Items, [this](auto* Source) { return OwnsSource(Source); });
	}
} // namespace Durin
