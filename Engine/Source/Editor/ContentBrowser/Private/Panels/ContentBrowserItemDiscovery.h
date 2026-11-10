#pragma once

#include "CoreMinimal.h"
#include "Threading/Task.h"

#include "Panels/ContentBrowserDataTypes.h"
#include "AssetRegistry/Catalog.h"
#include "Threading/TaskRuntime.h"

namespace Durin::Editor::ContentBrowser::Private
{
	struct FContentBrowserAssetSnapshot
	{
		uint64 Revision = 0;
		std::vector<FContentBrowserItem> Items;
	};
	// Serial collector over owned registry data. No live registry or editor state
	// is accessed during capture; a worker may exclusively own an instance.
	class FContentBrowserItemDiscovery
	{
	public:
		FContentBrowserItemDiscovery() = default;
		FContentBrowserItemDiscovery(const FContentBrowserItemDiscovery&) = delete;
		auto operator=(const FContentBrowserItemDiscovery&) -> FContentBrowserItemDiscovery& = delete;

		// Catalog must be non-null and captured on the registry-owning thread.
		auto CaptureItems(const std::string& PhysicalDirectory,
			bool bRecursive, std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog,
			const Tasks::FCancellationToken& Cancellation = {})
			-> FContentBrowserItemsSnapshot;
		auto CaptureDirectory(const std::string& PhysicalDirectory,
			std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog,
			const Tasks::FCancellationToken& Cancellation = {})
			-> FContentBrowserDirectorySnapshot;
		auto SetEntryStatusQueryForTesting(FEntryStatusQuery Query) -> void
		{
			EntryStatusQuery = std::move(Query);
		}

	private:
		auto QueryEntryStatus(const std::filesystem::directory_entry& Entry,
			std::error_code& Error) const -> std::filesystem::file_status;
		auto RefreshAssetDirectoryIndex(std::shared_ptr<const FContentBrowserAssetSnapshot> Catalog,
			const Tasks::FCancellationToken& Cancellation) -> void;
		auto AddEnumerationDiagnostic(EEnumerationDiagnosticKind Kind,
			const std::filesystem::path& Path, std::string Message) -> void;

		FContentBrowserItemsSnapshot Result;
		FEntryStatusQuery EntryStatusQuery;
		std::shared_ptr<const FContentBrowserAssetSnapshot> AssetCatalogSnapshot;
		// Pointers refer exclusively to the owned catalog, rebuilt on revision changes.
		std::unordered_map<std::string, std::vector<const FContentBrowserItem*>> AssetDirectoryIndex;
	};
}
