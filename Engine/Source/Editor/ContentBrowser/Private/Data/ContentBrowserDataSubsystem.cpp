#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserNativeDataSource.h"
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
		Builtins.emplace_back(NewObject<DContentBrowserAssetDataSource>(this, {}));
		Builtins.emplace_back(NewObject<DContentBrowserFileDataSource>(this, {}));
		for (auto& Source : Builtins)
			Source->Initialize();
		RefreshSources();
		ActivateDataSource(FName("Asset"));
		ActivateDataSource(FName("File"));
	}
	auto DContentBrowserDataSubsystem::Deinitialize() noexcept -> void
	{
		bStopping = true;
		for (auto& Source : Active)
			Source->SetDataSink(nullptr);
		Active.clear();
		Available.clear();
		for (auto& Source : Builtins)
			Source->Shutdown();
		Builtins.clear();
		PendingUpdates.clear();
		PublishedUpdates.clear();
		ItemDataUpdated.Clear();
		SourceRevision = ++Revision;
	}
	auto DContentBrowserDataSubsystem::RefreshSources() -> void
	{
		if (bStopping) return;
		const auto CurrentMountRevision = FMountPaths::GetMountRegistryRevision();
		if (MountRevision != CurrentMountRevision)
		{
			MountRevision = CurrentMountRevision;
			SourceRevision = ++Revision;
		}
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Next;
		FModularFeatureRegistry::Get().InvokeAll<DContentBrowserDataSource>([&](DContentBrowserDataSource& Source) {
			if (Source.GetOuter() == this || Source.GetOuter() == GetEditor()) Next.emplace_back(&Source);
		});
		Available = std::move(Next);
		std::erase_if(Active, [&](const auto& Source) {
			if (!Source->IsActive() || !std::ranges::any_of(Available, [&](const auto& S) { return S.Get() == Source.Get(); }))
			{
				Source->SetDataSink(nullptr);
				SourceRevision = ++Revision;
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
		SourceRevision = ++Revision;
		return true;
	}
	auto DContentBrowserDataSubsystem::DeactivateDataSource(FName Name) -> bool
	{
		auto* Source = GetActiveDataSource(Name);
		if (!Source) return false;
		Source->SetDataSink(nullptr);
		std::erase_if(Active, [Source](const auto& S) { return S.Get() == Source; });
		SourceRevision = ++Revision;
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
	auto DContentBrowserDataSubsystem::Tick(float DeltaSeconds) -> void
	{
		RefreshSources();
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get())) Source->Tick(DeltaSeconds);
		std::erase_if(PendingUpdates, [&](const auto& Update) { return !OwnsSource(Update.GetItemData().GetOwnerDataSource()); });
		PublishedFromRevision = PendingUpdates.empty() ? Revision : PendingFromRevision;
		PublishedToRevision = Revision;
		PublishedUpdates = std::move(PendingUpdates);
		PendingUpdates.clear();
		const auto Updates = PublishedUpdates;
		if (!Updates.empty()) ItemDataUpdated.Broadcast(Updates);
	}
	auto DContentBrowserDataSubsystem::QueueItemDataUpdate(FContentBrowserItemDataUpdate&& Update) -> void
	{
		if (!OwnsSource(Update.GetItemData().GetOwnerDataSource())) return;
		if (PendingUpdates.empty()) PendingFromRevision = Revision;
		PendingUpdates.push_back(std::move(Update));
		++Revision;
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
															   const FContentBrowserHostContext& Host) -> void
	{
		FContentBrowserDataCompiledFilter Compiled;
		CompileFilter(Path, Filter, Compiled);
		EnumerateItemsMatchingFilter(Compiled, Callback, Host);
	}
	auto DContentBrowserDataSubsystem::EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& Compiled,
																	const std::function<bool(FContentBrowserItem&&)>& Callback,
																	const FContentBrowserHostContext& Host) -> void

	{
		RefreshSources();
		const auto Sources = Active;
		std::vector<FContentBrowserItem> Items;
		for (const auto& Source : Sources)
		{
			if (!OwnsSource(Source.Get()))
				continue;
			const auto Entry = Compiled.CompiledFilters.find(Source.Get());
			if (Entry == Compiled.CompiledFilters.end() || Entry->second.Generation != Source->GetActivationGeneration() ||
				Entry->second.MountRevision != FMountPaths::GetMountRegistryRevision() ||
				Entry->second.DiscoveryRevision != Source->GetDiscoveryRevision())
				continue;
			Source->EnumerateItemsMatchingFilter(
				Compiled,
				[&](FContentBrowserItem&& Item)
				{
					if (!OwnsSource(Source.Get()))
						return false;
					MergeItem(Items, std::move(Item));
					return true;
				},
				Host);
		}
		for (auto& Item : Items)
		{
			std::erase_if(Item.InternalItems, [&](const auto& Data) { return !OwnsSource(Data.GetOwnerDataSource()); });
			if (!Item.InternalItems.empty() && !Callback(std::move(Item)))
				return;
		}
	}
	auto DContentBrowserDataSubsystem::EnumerateItemsAtPath(std::string_view Path, const std::function<bool(FContentBrowserItem&&)>& Callback, const FContentBrowserHostContext& Host) -> void
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
		std::map<DContentBrowserDataSource*, std::vector<FContentBrowserItemData>> Batches;
		if (Items.empty()) return false;
		for (const auto& Item : Items)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!OwnsSource(Source) || !Source->CanSaveItem(Item, Flags)) return false;
			Batches[Source].push_back(Item);
		}
		for (const auto& Item : Items)
			if (!OwnsSource(Item.GetOwnerDataSource())) return false;
		bool Result = true;
		for (const auto& [Source, Batch] : Batches)
		{
			const bool Current = std::ranges::all_of(Batch, [Source](const auto& Item) { return Item.GetOwnerDataSource() == Source; });
			Result = (Current && OwnsSource(Source) && Source->BulkSaveItems(Batch, Flags)) && Result;
		}
		return Result;
	}
	namespace
	{
		auto SupportsBatch(DContentBrowserDataSource* Source, std::span<const FContentBrowserItemData> Items) -> bool
		{
			if (!Source || Items.empty()) return false;
			if (std::ranges::all_of(Items, [Source](const auto& Item) { return Item.GetOwnerDataSource() == Source; })) return true;
			auto* Native = Cast<DContentBrowserNativeDataSource>(Source);
			return Native && Native->CanHandleNativeBatch(Items);
		}
	} // namespace
	auto DContentBrowserDataSubsystem::IsActive() const -> bool
	{
		return !bStopping && GetEditor() && !GetEditor()->IsShutdownRequested();
	}
	auto DContentBrowserDataSubsystem::GetRevision() -> uint64
	{
		RefreshSources();
		return Revision;
	}
	auto DContentBrowserDataSubsystem::CanCreateFolder(std::string_view Path, std::string* Error) -> bool
	{
		RefreshSources();
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get()) && Source->CanCreateFolder(Path, Error)) return true;
		return false;
	}
	auto DContentBrowserDataSubsystem::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserHostContext& Host) -> bool
	{
		RefreshSources();
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get()) && Source->CanCreateFolder(Path) && OwnsSource(Source.Get())) return Source->CreateFolder(Path, Out, Host);
		return false;
	}
	auto DContentBrowserDataSubsystem::PrepareDeleteItems(std::span<const FContentBrowserItemData> Items) -> FContentBrowserDeleteContext
	{
		if (Items.empty()) return {};
		auto* Source = Items.front().GetOwnerDataSource();
		if (!SupportsBatch(Source, Items)) return {};
		for (const auto& Item : Items)
			if (!OwnsSource(Item.GetOwnerDataSource())) return {};
		return Source->PrepareDeleteItems(Items);
	}

	auto DContentBrowserDataSubsystem::BulkCopyItems(std::span<const FContentBrowserItemData> Items, std::string_view Dest) -> bool
	{
		if (Items.empty()) return false;
		auto* Source = Items.front().GetOwnerDataSource();
		if (!OwnsSource(Source) || !SupportsBatch(Source, Items)) return false;
		for (const auto& Item : Items)
		{
			auto* Owner = Item.GetOwnerDataSource();
			if (!OwnsSource(Owner) || !Owner->CanCopyItem(Item, Dest)) return false;
		}
		for (const auto& Item : Items)
			if (!OwnsSource(Item.GetOwnerDataSource())) return false;
		return OwnsSource(Source) && Source->BulkCopyItems(Items, Dest);
	}
	auto DContentBrowserDataSubsystem::BulkMoveItems(std::span<const FContentBrowserItemData> Items, std::string_view Dest) -> bool
	{
		if (Items.empty()) return false;
		auto* Source = Items.front().GetOwnerDataSource();
		if (!OwnsSource(Source) || !SupportsBatch(Source, Items)) return false;
		for (const auto& Item : Items)
		{
			auto* Owner = Item.GetOwnerDataSource();
			if (!OwnsSource(Owner) || !Owner->CanMoveItem(Item, Dest)) return false;
		}
		for (const auto& Item : Items)
			if (!OwnsSource(Item.GetOwnerDataSource())) return false;
		return OwnsSource(Source) && Source->BulkMoveItems(Items, Dest);
	}
	auto DContentBrowserDataSubsystem::BulkDeleteItems(std::span<const FContentBrowserItemData> Items, FContentBrowserDeleteContext& Context) -> bool
	{
		auto* Source = Context.OwnerItem.GetOwnerDataSource();
		if (!OwnsSource(Source) || !SupportsBatch(Source, Items)) return false;
		for (const auto& Item : Items)
			if (!OwnsSource(Item.GetOwnerDataSource())) return false;
		return Source->BulkDeleteItems(Items, Context);
	}
} // namespace Durin
