#include "ContentBrowserDataSubsystem.h"
#include "Data/ContentBrowserSaveDispatch.h"
#include "Data/ContentBrowserTransferDispatch.h"
#include "Data/ContentBrowserDeleteRequestLease.h"
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
		RefreshSources();
	}
	auto DContentBrowserDataSubsystem::Deinitialize() noexcept -> void
	{
		bStopping = true;
		DContentBrowserDataSource::OnDataSourceMembershipChanged().Remove(SourceMembershipChanged);
		for (auto& Source : Active)
			Source->SetDataSink(nullptr);
		Active.clear();
		Available.clear();
		PendingUpdates.clear();
		PendingUpdateRevisions.clear();
		PublishedUpdates.clear();
		PublishedUpdateRevisions.clear();
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
		if (Available.size() != Next.size() || !std::ranges::equal(Available, Next, [](const auto& A, const auto& B) { return A.Get() == B.Get(); })) SourceRevision = ++Revision;
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
				PendingUpdateRevisions.erase(PendingUpdateRevisions.begin() + I);
			}
		PublishedFromRevision = PendingUpdates.empty() ? Revision : PendingFromRevision;
		PublishedToRevision = Revision;
		PublishedUpdates = std::move(PendingUpdates);
		PublishedUpdateRevisions = std::move(PendingUpdateRevisions);
		PendingUpdates.clear();
		PendingUpdateRevisions.clear();
		const auto Updates = PublishedUpdates;
		if (!Updates.empty()) ItemDataUpdated.Broadcast(Updates);
	}
	auto DContentBrowserDataSubsystem::QueueItemDataUpdate(FContentBrowserItemDataUpdate&& Update) -> void
	{
		if (!OwnsSource(Update.GetItemData().GetOwnerDataSource())) return;
		if (PendingUpdates.empty()) PendingFromRevision = Revision;
		PendingUpdates.push_back(std::move(Update));
		PendingUpdateRevisions.push_back(++Revision);
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
	auto DContentBrowserDataSubsystem::BulkSaveItems(std::span<const FContentBrowserItemData> Items, EContentBrowserItemSaveFlags Flags) -> FContentBrowserOperationResult
	{
		RefreshSources();
		return ContentBrowser::Private::DispatchSaveItems(Items, Flags, [this](auto* Source) { return OwnsSource(Source); });
	}

	namespace
	{
		struct FCompositeDeleteRequest final : IContentBrowserDeleteRequest
		{
			struct FPartition
			{
				TStrongObjectPtr<DContentBrowserDataSource> Source;
				std::string Name;
				std::vector<FContentBrowserItemData> Items;
				FContentBrowserDeleteContext Context;
			};
			std::vector<FPartition> Partitions;
			std::vector<FContentBrowserItemData> Captured;
			mutable std::vector<std::shared_ptr<const FContentBrowserDeletePreview>> Previous;
			mutable std::shared_ptr<FContentBrowserDeletePreview> Preview;
			auto CanExecute() const -> bool override
			{ return !Partitions.empty() && std::ranges::all_of(Partitions, [](const auto& Part) { return Part.Context.CanExecute(); }); }
			auto GetPreview() const -> std::shared_ptr<const FContentBrowserDeletePreview> override
			{
				std::vector<std::shared_ptr<const FContentBrowserDeletePreview>> Current;
				for (const auto& Part : Partitions) Current.push_back(Part.Context.Request ? Part.Context.Request->GetPreview() : nullptr);
				if (Preview && Current == Previous) return Preview;
				Previous = Current;
				Preview = std::make_shared<FContentBrowserDeletePreview>(); Preview->DisplayName = "selected items";
				for (const auto& Value : Current) if (Value)
				{
					Preview->Summary.AssetCount += Value->Summary.AssetCount; Preview->Summary.FileCount += Value->Summary.FileCount;
					Preview->Summary.CompanionCount += Value->Summary.CompanionCount; Preview->Summary.FolderCount += Value->Summary.FolderCount;
					Preview->Blockers.insert(Preview->Blockers.end(), Value->Blockers.begin(), Value->Blockers.end());
					Preview->Warnings.insert(Preview->Warnings.end(), Value->Warnings.begin(), Value->Warnings.end());
				}
				return Preview;
			}
		};
	}
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
	auto DContentBrowserDataSubsystem::CreateFolder(std::string_view Path, FContentBrowserItemDataTemporaryContext& Out, const FContentBrowserOperationContext& Host) -> bool
	{
		RefreshSources();
		const auto Sources = Active;
		for (const auto& Source : Sources)
			if (OwnsSource(Source.Get()) && Source->CanCreateFolder(Path) && OwnsSource(Source.Get())) return Source->CreateFolder(Path, Out, Host);
		return false;
	}
	auto DContentBrowserDataSubsystem::PrepareDeleteItems(std::span<const FContentBrowserItemData> Items) -> FContentBrowserDeleteContext
	{
		RefreshSources();
		if (Items.empty()) return {};
		auto Request = std::make_unique<FCompositeDeleteRequest>();
		Request->Captured.assign(Items.begin(), Items.end());
		for (const auto& Item : Items)
		{
			auto* Source = Item.GetOwnerDataSource();
			if (!OwnsSource(Source)) return {};
			auto Part = std::ranges::find_if(Request->Partitions, [Source](const auto& Value) { return Value.Source.Get() == Source; });
			if (Part == Request->Partitions.end()) Request->Partitions.push_back({TStrongObjectPtr<DContentBrowserDataSource>(Source), {}, {Item}, {}});
			else if (std::ranges::none_of(Part->Items, [&](const auto& Value) { return Value.GetContributionKey() == Item.GetContributionKey(); })) Part->Items.push_back(Item);
		}
		for (auto& Part : Request->Partitions)
		{
			Part.Name = Part.Source->GetDataSourceName().ToString();
			std::ranges::sort(Part.Items, {}, &FContentBrowserItemData::GetVirtualPath);
		}
		std::ranges::sort(Request->Partitions, {}, &FCompositeDeleteRequest::FPartition::Name);
		for (auto& Part : Request->Partitions)
		{
			if (!OwnsSource(Part.Source.Get()) || std::ranges::any_of(Part.Items, [&](const auto& Item) { return Item.GetOwnerDataSource() != Part.Source.Get(); })) return {};
			Part.Context = Part.Source->PrepareDeleteItems(Part.Items);
			if (!Part.Context.Request) return {};
		}
		return {Items.front(), std::move(Request)};
	}

	auto DContentBrowserDataSubsystem::BulkCopyItems(std::span<const FContentBrowserItemData> Items, std::string_view Dest) -> FContentBrowserOperationResult
	{
		TStrongObjectPtr<DContentBrowserDataSubsystem> Lifetime(this);
		RefreshSources();
		return ContentBrowser::Private::DispatchTransferItems(Items, Dest, false, [this](auto* Source) { return OwnsSource(Source); });
	}
	auto DContentBrowserDataSubsystem::BulkMoveItems(std::span<const FContentBrowserItemData> Items, std::string_view Dest) -> FContentBrowserOperationResult
	{
		TStrongObjectPtr<DContentBrowserDataSubsystem> Lifetime(this);
		RefreshSources();
		return ContentBrowser::Private::DispatchTransferItems(Items, Dest, true, [this](auto* Source) { return OwnsSource(Source); });
	}
	auto DContentBrowserDataSubsystem::BulkDeleteItems(std::span<const FContentBrowserItemData> Items, FContentBrowserDeleteContext& Context) -> FContentBrowserOperationResult
	{
		TStrongObjectPtr<DContentBrowserDataSubsystem> Lifetime(this);
		const std::vector<FContentBrowserItemData> Snapshot(Items.begin(), Items.end());
		Items = Snapshot;
		FContentBrowserOperationResult Result;
		auto Reject = [&](std::string Error) {
			Result.bStopped = true;
			for (const auto& Item : Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, Error);
			return Result;
		};
		ContentBrowser::Private::FContentBrowserDeleteRequestLease Lease(Context);
		auto* Request = dynamic_cast<FCompositeDeleteRequest*>(Lease.Request.get());
		if (!Request)
		{
			auto* Owner = Lease.Owner.GetOwnerDataSource();
			if (!OwnsSource(Owner) || std::ranges::any_of(Items, [Owner](const auto& Item) { return Item.GetOwnerDataSource() != Owner; })) return Reject("Deletion scope is not owned by the confirmed source.");
			TStrongObjectPtr<DContentBrowserDataSource> OwnerLifetime(Owner);
			FContentBrowserDeleteContext Local{Lease.Owner, std::move(Lease.Request)};
			const auto Native = Owner->BulkDeleteItems(Items, Local);
			Lease.Request = std::move(Local.Request); Lease.bConsumed = !Lease.Request;
			return Native;
		}
		if (Items.size() != Request->Captured.size()) return Reject("Deletion scope differs from confirmation.");
		for (size_t I = 0; I < Items.size(); ++I)
			if (Items[I].GetContributionKey() != Request->Captured[I].GetContributionKey() || Items[I].GetPayload() != Request->Captured[I].GetPayload()) return Reject("Deletion scope differs from confirmation.");
		auto Current = [&](const auto& Part) { return OwnsSource(Part.Source.Get()) && std::ranges::all_of(Part.Items, [&](const auto& Item) { return Item.GetOwnerDataSource() == Part.Source.Get(); }); };
		for (auto& Part : Request->Partitions)
		{
			if (!Current(Part) || !Part.Context.CanExecute()) return Reject("Deletion admission was withdrawn or blocked.");
			const auto Validation = Part.Context.Request->ValidateForExecution();
			if (Validation == EContentBrowserDeleteValidation::Stale)
			{
				Context = PrepareDeleteItems(Items);
				return Reject("Deletion confirmation changed; confirm the replacement before retrying.");
			}
			if (Validation != EContentBrowserDeleteValidation::Valid) return Reject("Deletion validation rejected the confirmed scope.");
		}
		for (const auto& Part : Request->Partitions) if (!Current(Part)) return Reject("Deletion admission changed before destruction.");
		bool Consumed = false;
		for (auto& Part : Request->Partitions)
		{
			if (!Current(Part)) Result.bStopped = true;
			if (Result.bStopped || Result.bCancelled)
				for (const auto& Item : Part.Items) Result.Add(Item.GetContributionKey(), EContentBrowserEffectStatus::Unattempted, "Deletion stopped before this source partition.");
			else
			{
				Result.Append(Part.Source->BulkDeleteItems(Part.Items, Part.Context));
				Consumed |= !Part.Context.Request;
			}
		}
		Lease.bConsumed = Consumed || Result.HasCommittedEffects();
		return Result;
	}

} // namespace Durin
