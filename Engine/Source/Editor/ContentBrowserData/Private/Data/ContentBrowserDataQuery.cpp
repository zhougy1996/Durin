#include "ContentBrowserDataQuery.h"
#include "CoreGlobals.h"
namespace Durin::ContentBrowser::Private
{
	FContentBrowserDataQuery::FContentBrowserDataQuery(TWeakObjectPtr<DContentBrowserDataSubsystem> InService, FContentBrowserOperationContext InPolicy)
		: Service(InService), Policy(std::move(InPolicy))
	{
		if (auto* Data = Service.Get())
		{
			const std::weak_ptr<void> Alive = Lifetime;
			Updated = Data->OnItemDataUpdated().AddLambda([this, Alive](std::span<const FContentBrowserItemDataUpdate> Updates) {
				if (Alive.expired()) return;
				std::erase_if(PendingCaptures, [](const auto& Capture) { return Capture.Cancellation.expired(); });
				for (const auto& Capture : PendingCaptures)
					for (const auto& Update : Updates)
					{
						auto Touches = [&](std::string_view Path) {
							const auto Scope = std::filesystem::path(Capture.Scope).lexically_normal().generic_string();
							auto Item = std::filesystem::path(Path).lexically_normal().generic_string();
							while (Item.size() > 1 && Item.back() == '/') Item.pop_back();
							return Item == Scope || std::filesystem::path(Item).parent_path().generic_string() == Scope
								|| (Capture.Recursive && Item.starts_with(Scope == "/" ? Scope : Scope + "/"))
								|| (Update.GetItemData().GetItemType() == EContentBrowserItemFlags::Type_Folder && Scope.starts_with(Item == "/" ? Item : Item + "/"));
						};
						if (Touches(Update.GetItemData().GetVirtualPath()) || (!Update.GetPreviousVirtualPath().empty() && Touches(Update.GetPreviousVirtualPath())))
							if (auto Token = Capture.Cancellation.lock()) Token->Cancel();
					}
			});
			Refreshed = Data->OnItemDataRefreshed().AddLambda([this, Alive] {
				if (!Alive.expired()) for (const auto& Capture : PendingCaptures) if (auto Token = Capture.Cancellation.lock()) Token->Cancel();
			});
		}
	}
	FContentBrowserDataQuery::~FContentBrowserDataQuery()
	{
		Lifetime.reset();
		if (auto* Data = Service.Get()) { Data->OnItemDataUpdated().Remove(Updated); Data->OnItemDataRefreshed().Remove(Refreshed); }
	}
	auto FContentBrowserDataQuery::FindSourceKey(FName Name) const -> FObjectKey
	{
		for (const auto& Source : GetSources())
			if (Source->GetDataSourceName() == Name) return FObjectKey(Source.Get());
		return {};
	}

	auto FContentBrowserDataQuery::GetSources() const -> std::vector<TStrongObjectPtr<DContentBrowserDataSource>>
	{
		if (!IsAcceptingRequests())
			return {};
		TStrongObjectPtr<DContentBrowserDataSubsystem> DataService(Service.Get());
		DataService->RefreshSources();
		return DataService->Active;
	}
	auto FContentBrowserDataQuery::OwnsSource(const DContentBrowserDataSource* Source) const -> bool
	{
		auto* Data = Service.Get();
		return bAccepting && Data && Data->OwnsSource(Source);
	}
	auto FContentBrowserDataQuery::IsAcceptingRequests() const -> bool
	{
		return bAccepting && Service.Get() && Service.Get()->IsActive();
	}
	auto FContentBrowserDataQuery::PrepareItemsCapture(std::string Virtual, bool Recursive, FEntryStatusQuery Query,
													   std::shared_ptr<Tasks::FCancellationToken> Cancellation) const -> FItemsCapture
	{
		if (!IsAcceptingRequests())
			return {};
		const std::weak_ptr<void> Alive = Lifetime;
		const auto Sources = GetSources();
		if (Alive.expired()) return {};
		if (!Cancellation) Cancellation = std::make_shared<Tasks::FCancellationToken>();
		std::erase_if(PendingCaptures, [](const auto& Capture) { return Capture.Cancellation.expired(); });
		auto Scope = std::filesystem::path(Virtual).lexically_normal().generic_string();
		while (Scope.size() > 1 && Scope.back() == '/') Scope.pop_back();
		PendingCaptures.push_back({Cancellation, std::move(Scope), Recursive});
		struct FJob
		{
			TWeakObjectPtr<DContentBrowserDataSource> Source;
			std::shared_ptr<const IContentBrowserDataCompiledFilter> Filter;
			std::vector<FContentBrowserItemData> Items;
		};
		std::vector<FJob> Jobs;
		for (const auto& Source : Sources)
		{
			if (!OwnsSource(Source.Get()))
				continue;
			FJob Job{.Source = Source.Get()};
			FContentBrowserDataCompiledFilter Compiled;
			Source->CompileFilter(Virtual, {.bRecursive = Recursive}, Compiled);
			if (Alive.expired() || Cancellation->IsCanceled()) { Cancellation->Cancel(); return {}; }
			if (!OwnsSource(Source.Get())) continue;
			if (const auto It = Compiled.CompiledFilters.find(Source.Get()); It != Compiled.CompiledFilters.end())
				Job.Filter = It->second;
			if (!Job.Filter)
				Source->EnumerateItemsMatchingFilter(
					Compiled,
					[&](FContentBrowserItemData&& Item)
					{
						if (Alive.expired() || Cancellation->IsCanceled()) return false;
						if (Item.GetOwnerDataSource() == Source.Get()) Job.Items.push_back(std::move(Item));
						return true;
					});
			if (Alive.expired() || Cancellation->IsCanceled()) { Cancellation->Cancel(); return {}; }
			if (OwnsSource(Source.Get())) Jobs.push_back(std::move(Job));
		}
		if (!IsAcceptingRequests()) { Cancellation->Cancel(); return {}; }
		return [Jobs = std::move(Jobs), Query = std::move(Query), Cancellation = std::move(Cancellation)]()
		{
			auto Result = std::make_unique<FContentBrowserItemsCapture>();
			Result->Cancellation = Cancellation;
			for (const auto& Job : Jobs)
			{
				if (Cancellation->IsCanceled())
					break;
				auto Data = Job.Filter ? Job.Filter->Capture(*Cancellation, Query) : FContentBrowserDataCapture{.Items = Job.Items};
				const auto Begin = Result->Items.size();
				Result->Items.insert(Result->Items.end(), std::make_move_iterator(Data.Items.begin()),
									 std::make_move_iterator(Data.Items.end()));
				Result->Contributions.push_back({Job.Source, Begin, Result->Items.size()});
				for (auto& Diagnostic : Data.Diagnostics)
					if (Result->Diagnostics.size() < 8)
						Result->Diagnostics.push_back(std::move(Diagnostic));
					else
						++Result->SuppressedDiagnosticCount;
				Result->SuppressedDiagnosticCount += Data.SuppressedDiagnosticCount;
			}
			return Result;
		};
	}
	auto FContentBrowserDataQuery::PrepareDirectoryCapture(std::string Virtual, FEntryStatusQuery Query,
														   std::shared_ptr<Tasks::FCancellationToken> Cancellation) const
		-> FDirectoryCapture
	{
		auto Work = PrepareItemsCapture(std::move(Virtual), false, std::move(Query), std::move(Cancellation));
		if (!Work)
			return {};
		return [Work = std::move(Work)]() mutable
		{
			auto Result = std::make_unique<FContentBrowserDirectorySnapshot>();
			Result->Capture = Work();
			// Keep the standalone prepared result useful without its original query.
			for (const auto& Item : Result->Capture->Items)
				if (Item.GetItemType() == EContentBrowserItemFlags::Type_Folder)
					Result->Children.emplace_back(Item.GetVirtualPath());
			return Result;
		};
	}
	auto FContentBrowserDataQuery::ProjectItems(FContentBrowserItemsCapture Snapshot, std::string_view Path, bool Recursive) const
		-> FContentBrowserItemsSnapshot
	{
		FContentBrowserItemsSnapshot Result;
		const std::weak_ptr<void> Alive = Lifetime;
		const auto BindingPolicy = Policy;
		if (Snapshot.Cancellation && Snapshot.Cancellation->IsCanceled()) { Result.bStale = true; return Result; }
		Result.Diagnostics = std::move(Snapshot.Diagnostics);
		Result.SuppressedDiagnosticCount = Snapshot.SuppressedDiagnosticCount;
		for (const auto& Contribution : Snapshot.Contributions)
		{
			auto* Source = Contribution.Source.Get();
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
			if (!OwnsSource(Source))
			{ Result.bStale = true; Result.Items.clear(); return Result; }
			for (size_t Index = Contribution.Begin; Index < Contribution.End; ++Index)
			{
				auto Data = Source->BindItemData(Snapshot.Items[Index], BindingPolicy);
				if (Alive.expired() || (Snapshot.Cancellation && Snapshot.Cancellation->IsCanceled()) || !OwnsSource(Source))
				{
					Result.bStale=true;
					Result.Items.clear();
					return Result;
				}
				if (Data.GetOwnerDataSource() != Source)
					continue;
				DContentBrowserDataSubsystem::MergeItem(Result.Items, FContentBrowserItem{.InternalItems = {std::move(Data)}});
			}
		}

		return Result;
	}
	auto FContentBrowserDataQuery::ProjectDirectory(FContentBrowserDirectorySnapshot Snapshot, std::string_view Virtual) const
		-> FContentBrowserDirectorySnapshot
	{
		if (!Snapshot.Capture)
			return Snapshot;
		auto Data = ProjectItems(std::move(*Snapshot.Capture), Virtual, false);
		Snapshot.Capture.reset();
		Snapshot.Children.clear();
		Snapshot.bStale = Data.bStale;
		Snapshot.Diagnostics = std::move(Data.Diagnostics);
		Snapshot.SuppressedDiagnosticCount = Data.SuppressedDiagnosticCount;
		for (const auto& Item : Data.Items)
			if (Item.IsFolder())
				{ std::string Physical; Item.GetItemPhysicalPath(Physical); Snapshot.Children.emplace_back(Physical.empty() ? Item.GetVirtualPath() : Physical); }
		std::ranges::sort(Snapshot.Children);
		Snapshot.Children.erase(std::unique(Snapshot.Children.begin(), Snapshot.Children.end()), Snapshot.Children.end());
		return Snapshot;
	}
	auto FContentBrowserDataQuery::IsProviderDirectory(std::string_view Virtual) const -> bool
	{
		std::string Target(Virtual);
		while (Target.size() > 1 && Target.back() == '/')
			Target.pop_back();
		const auto Slash = Target.find_last_of('/');
		if (Slash == std::string::npos)
			return false;
		auto Work = PrepareItemsCapture(Target.substr(0, Slash + 1), false);
		if (!Work)
			return false;
		auto Data = ProjectItems(std::move(*Work()), {}, false);
		return std::ranges::any_of(Data.Items,
								   [&](const auto& Item)
								   {
									   auto Path = Item.GetVirtualPath();
									   while (Path.size() > 1 && Path.back() == '/')
										   Path.pop_back();
									   return Item.IsFolder() && Path == Target;
								   });
	}
} // namespace Durin::ContentBrowser::Private
