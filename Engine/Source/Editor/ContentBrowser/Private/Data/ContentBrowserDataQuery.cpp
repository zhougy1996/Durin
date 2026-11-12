#include "Data/ContentBrowserItemAttributeHelpers.h"
#include "Data/ContentBrowserDataQuery.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"
#include "CoreGlobals.h"
namespace Durin::ContentBrowser::Private
{
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
		if (!bServiceBound)
			return StandaloneSources;
		auto* DataService = Service.Get();
		(void)DataService->GetRevision();
		return DataService->Active;
	}
	auto FContentBrowserDataQuery::OwnsSource(const DContentBrowserDataSource* Source) const -> bool
	{
		if (!IsAcceptingRequests() || !Source || !Source->IsActive())
			return false;
		if (bServiceBound)
			return Service.Get()->GetActiveDataSource(Source->GetDataSourceName()) == Source;
		return std::ranges::any_of(StandaloneSources, [&](const auto& Entry) { return Entry.Get() == Source; });
	}
	auto FContentBrowserDataQuery::IsAcceptingRequests() const -> bool
	{
		return bAccepting && (!bServiceBound || (Service.Get() && Service.Get()->IsActive()));
	}
	auto FContentBrowserDataQuery::PrepareItemsCapture(std::string Virtual, bool Recursive, FEntryStatusQuery Query,
													   std::shared_ptr<Tasks::FCancellationToken> Cancellation) const -> FItemsCapture
	{
		if (!IsAcceptingRequests())
			return {};
		struct FJob
		{
			TWeakObjectPtr<DContentBrowserDataSource> Source;
			uint64 Generation = 0, Mount = 0;
			std::string Project;
			std::shared_ptr<const IContentBrowserDataCompiledFilter> Filter;
			std::vector<FContentBrowserItem> Items;
		};
		const auto CaptureRevision = GetRevision();
		std::vector<FJob> Jobs;
		for (const auto& Source : GetSources())
		{
			if (!OwnsSource(Source.Get()))
				continue;
			FJob Job{.Source = Source.Get(),
					 .Generation = Source->GetActivationGeneration(),
					 .Mount = FMountPaths::GetMountRegistryRevision(),
					 .Project = FPaths::ProjectFile()};
			FContentBrowserDataCompiledFilter Compiled;
			Source->CompileFilter(Virtual, {.bRecursive = Recursive}, Compiled);
			if (const auto It = Compiled.CompiledFilters.find(Source.Get()); It != Compiled.CompiledFilters.end())
				Job.Filter = It->second.Data;
			if (!Job.Filter)
				Source->EnumerateItemsMatchingFilter(
					Compiled,
					[&](FContentBrowserItem&& Item)
					{
						Job.Items.push_back(std::move(Item));
						return true;
					},
					Policy);
			Jobs.push_back(std::move(Job));
		}
		if (!Cancellation)
			Cancellation = std::make_shared<Tasks::FCancellationToken>();
		return [CaptureRevision, Jobs = std::move(Jobs), Query = std::move(Query), Cancellation = std::move(Cancellation)]()
		{
			auto Result = std::make_unique<FContentBrowserItemsSnapshot>();
			Result->QueryRevision = CaptureRevision;
			for (const auto& Job : Jobs)
			{
				if (Cancellation->IsCanceled())
					break;
				auto Data = Job.Filter ? Job.Filter->Capture(*Cancellation, Query) : FContentBrowserDataCapture{.Items = Job.Items};
				const auto Begin = Result->Items.size();
				Result->Items.insert(Result->Items.end(), std::make_move_iterator(Data.Items.begin()),
									 std::make_move_iterator(Data.Items.end()));
				Result->Contributions.push_back({Job.Source, Job.Generation, Job.Mount, Begin, Result->Items.size(), Job.Project});
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
				if (Item.IsFolder())
					Result->Children.emplace_back(Item.GetVirtualPath());
			return Result;
		};
	}
	auto FContentBrowserDataQuery::ProjectItems(FContentBrowserItemsSnapshot Snapshot, std::string_view Path, bool Recursive) const
		-> FContentBrowserItemsSnapshot
	{
		FContentBrowserItemsSnapshot Result;
		Result.QueryRevision = Snapshot.QueryRevision;
		if (Snapshot.QueryRevision && IsAffectedByUpdates(Snapshot.QueryRevision, Path, Recursive))
		{
			Result.bStale = true;
			return Result;
		}
		Result.Diagnostics = std::move(Snapshot.Diagnostics);
		Result.SuppressedDiagnosticCount = Snapshot.SuppressedDiagnosticCount;
		for (const auto& Contribution : Snapshot.Contributions)
		{
			auto* Source = Contribution.Source.Get();
			TStrongObjectPtr<DContentBrowserDataSource> KeepAlive(Source);
			if (!OwnsSource(Source) || Source->GetActivationGeneration() != Contribution.Generation ||
				FMountPaths::GetMountRegistryRevision() != Contribution.MountRevision || FPaths::ProjectFile() != Contribution.Project)
				continue;
			for (size_t Index = Contribution.Begin; Index < Contribution.End; ++Index)
			{
				auto Item = std::move(Snapshot.Items[Index]);
				if (Item.InternalItems.empty() || !Item.InternalItems.front().GetOwnerDataSource())
					Item.InternalItems = {Source->CreateItemData(Item, Policy)};
				if (!OwnsSource(Source) || Source->GetActivationGeneration()!=Contribution.Generation ||
					FMountPaths::GetMountRegistryRevision()!=Contribution.MountRevision || FPaths::ProjectFile()!=Contribution.Project)
				{
					Result.bStale=true;
					Result.Items.clear();
					return Result;
				}
				std::erase_if(Item.InternalItems, [&](const auto& Data) { return !OwnsSource(Data.GetOwnerDataSource()); });
				if (Item.InternalItems.empty())
					continue;
				DContentBrowserDataSubsystem::MergeItem(Result.Items, std::move(Item));
			}
		}
		// Explicitly injected presentation snapshots carry no provider work.
		if (Snapshot.Contributions.empty() && IsAcceptingRequests())
		{
			for (auto& Item : Snapshot.Items)
			{
				for (const auto& Source : GetSources())
				{
					if (!OwnsSource(Source.Get()))
						continue;
					auto Data = Source->CreateItemData(Item, Policy);
					if (Data.GetOwnerDataSource())
					{
						Item.InternalItems = {std::move(Data)};
						break;
					}
				}
				Result.Items.push_back(std::move(Item));
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
		Snapshot.QueryRevision = Data.QueryRevision;
		Snapshot.bStale = Data.bStale;
		Snapshot.Diagnostics = std::move(Data.Diagnostics);
		Snapshot.SuppressedDiagnosticCount = Data.SuppressedDiagnosticCount;
		for (const auto& Item : Data.Items)
			if (Item.IsFolder())
				Snapshot.Children.emplace_back(ItemAttributes::PhysicalPath(Item).empty() ? Item.GetVirtualPath() : ItemAttributes::PhysicalPath(Item));
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
	auto FContentBrowserDataQuery::GetRevision() const -> uint64
	{
		FRevision Current;
		for (const auto& Source : GetSources())
			if (OwnsSource(Source.Get()))
				Current.Sources.push_back({Source.Get(), Source->GetActivationGeneration(), Source->GetDiscoveryRevision()});
		// Provider revision queries can publish committed metadata events. Capture the
		// service cursor after those callbacks so the snapshot covers its own baseline.
		if (auto* DataService = Service.Get()) Current.Service = DataService->GetRevision();
		if (!Revisions.empty())
		{
			const auto& Last = Revisions.rbegin()->second;
			if (Last.Service == Current.Service && Last.Sources.size() == Current.Sources.size() &&
				std::ranges::equal(
					Last.Sources, Current.Sources, [](const auto& A, const auto& B)
					{ return A.Source.Get() == B.Source.Get() && A.Generation == B.Generation && A.Revision == B.Revision; }))
				return Revision;
		}
		Revisions[++Revision] = std::move(Current);
		while (Revisions.size() > 16)
			Revisions.erase(Revisions.begin());
		return Revision;
	}
	auto FContentBrowserDataQuery::IsAffectedByUpdates(uint64 Since, std::string_view Directory, bool Recursive) const -> bool
	{
		const auto Current = GetRevision();
		if (Since == Current)
			return false;
		const auto It = Revisions.find(Since);
		if (It == Revisions.end())
			return true;
		const auto& Before = It->second;
		const auto& After = Revisions.at(Current);
		if (Before.Sources.size() != After.Sources.size())
			return true;
		for (size_t I = 0; I < Before.Sources.size(); ++I)
		{
			const auto& A = Before.Sources[I];
			const auto& B = After.Sources[I];
			if (A.Source.Get() != B.Source.Get() || A.Generation != B.Generation)
				return true;
			if (A.Revision != B.Revision && (!B.Source.Get() || B.Source.Get()->IsDiscoveryPathAffected(A.Revision, Directory, Recursive)))
				return true;
		}
		auto* DataService = Service.Get();
		if (!DataService || Before.Service == After.Service)
			return false;
		Since = Before.Service;
		if (Since < DataService->SourceRevision)
			return true;
		auto Coverage = DataService->PendingUpdates.empty() ? After.Service : DataService->PendingFromRevision;
		if (!DataService->PublishedUpdates.empty() && DataService->PublishedToRevision >= Coverage)
			Coverage = DataService->PublishedFromRevision;
		if (Since < Coverage)
			return true;
		auto Affected = [&](const auto& Update)
		{
			auto Touches = [&](std::string_view Path)
			{
				auto P = std::filesystem::path(Path).lexically_normal().generic_string();
				auto Scope = std::filesystem::path(Directory).lexically_normal().generic_string();
				while (Scope.size() > 1 && Scope.back() == '/')
					Scope.pop_back();
				return P == Scope || std::filesystem::path(P).parent_path().generic_string() == Scope ||
					   (Recursive && P.starts_with(Scope + "/")) ||
					   (Update.GetItemData().GetItemType() == EContentBrowserItemFlags::Type_Folder && Scope.starts_with(P + "/"));
			};
			return Touches(Update.GetItemData().GetVirtualPath()) ||
				   (!Update.GetPreviousVirtualPath().empty() && Touches(Update.GetPreviousVirtualPath()));
		};
		for (size_t I = 0; I < DataService->PendingUpdates.size(); ++I)
			if (DataService->PendingUpdateRevisions[I] > Since && Affected(DataService->PendingUpdates[I])) return true;
		for (size_t I = 0; I < DataService->PublishedUpdates.size(); ++I)
			if (DataService->PublishedUpdateRevisions[I] > Since && Affected(DataService->PublishedUpdates[I])) return true;
		return false;
	}
} // namespace Durin::ContentBrowser::Private
