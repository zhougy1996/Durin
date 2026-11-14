#pragma once
#include "CoreMinimal.h"
#include "Panels/ContentBrowserModel.h"
namespace Durin::ContentBrowser::Private
{
	// Presentation-only snapshots live in tests; production queries accept provider captures only.
	struct FContentBrowserDataQueryFixtureAccess
	{
		static auto Bind(FContentBrowserDataQuery& Query, std::vector<FContentBrowserItem> Items) -> std::vector<FContentBrowserItem>
		{
			std::vector<FContentBrowserItem> Bound;
			for (auto& Item : Items) for (const auto& Source : Query.GetSources())
			{
				auto Data=Source->BindItemData(*Item.GetPrimaryInternalItem(),Query.Policy);
				if (Data.GetOwnerDataSource()) { Item.InternalItems={std::move(Data)}; Bound.push_back(std::move(Item)); break; }
			}
			return Bound;
		}
	};
	struct FContentBrowserModelFixtureAccess
	{
		static auto Wait(FContentBrowserModel& Model) -> void
		{
			const std::weak_ptr<void> Lifetime = Model.EventLifetime;
			do
			{
				Model.PumpPendingSnapshots();
				if (Lifetime.expired()) return;
				if (Model.ItemsTask.IsValid()) (void)WaitTask(Tasks::FTask(Model.ItemsTask));
				if (Model.TreeTask.IsValid()) (void)WaitTask(Tasks::FTask(Model.TreeTask));
			} while (Model.ItemsTask.IsValid() || Model.TreeTask.IsValid() || Model.IsLoading()
				|| !Model.RequestedDirectoryChildrenSnapshots.empty());
		}
		static auto Request(const FContentBrowserModel& Model) -> std::shared_ptr<const void> { return Model.ItemsRequest; }
		static auto SetSnapshot(FContentBrowserModel& Model, std::string Directory, std::vector<FContentBrowserItem> Items) -> void
		{
			Model.CancelPendingSnapshots();
			Model.Session.CurrentPhysicalPath=std::filesystem::path(Directory).lexically_normal().generic_string();
			Model.Session.CurrentVirtualPath=Model.PhysicalToVirtualDirectory(Model.Session.CurrentPhysicalPath);
			Model.PublishedSnapshot=std::make_shared<const FContentBrowserItemsSnapshot>(FContentBrowserItemsSnapshot{.Items=FContentBrowserDataQueryFixtureAccess::Bind(Model.DataQuery,std::move(Items))});
			Model.DirectoryChildrenCache.clear(); Model.RequestedDirectoryChildrenSnapshots.clear(); Model.EnumerationDiagnostics.clear(); Model.SuppressedEnumerationDiagnosticCount=0; Model.RebuildItems();
		}
		static auto Search(FContentBrowserModel& Model, std::string_view Search) -> void { Model.Session.Query.Search=Search; Model.RebuildItems(); }
	};
}
