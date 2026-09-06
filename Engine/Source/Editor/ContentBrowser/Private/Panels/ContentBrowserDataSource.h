#pragma once

#include "Panels/ContentBrowserDataTypes.h"
#include "AssetRegistry/Catalog.h"
#include <unordered_map>
#include "Threading/Task.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Serial collector over owned registry data. No live registry or editor state
	// is accessed during capture; a worker may exclusively own an instance.
	class FContentBrowserDataSource
	{
	public:
		FContentBrowserDataSource() = default;
		FContentBrowserDataSource(const FContentBrowserDataSource&) = delete;
		auto operator=(const FContentBrowserDataSource&) -> FContentBrowserDataSource& = delete;

		// Catalog must be non-null and captured on the registry-owning thread.
		auto CaptureItems(const std::string& PhysicalDirectory,
			bool bRecursive, std::shared_ptr<const FAssetCatalogSnapshot> Catalog,
			const FTaskCancellationToken& Cancellation = {})
			-> FContentBrowserItemsSnapshot;
		auto CaptureDirectory(const std::string& PhysicalDirectory,
			const FTaskCancellationToken& Cancellation = {})
			-> FContentBrowserDirectorySnapshot;
		auto SetEntryStatusQueryForTesting(FEntryStatusQuery Query) -> void
		{
			EntryStatusQuery = std::move(Query);
		}

	private:
		struct FIndexedAsset
		{
			const FPackagePath* Path = nullptr;
			const FAssetData* Data = nullptr;
		};
		auto QueryEntryStatus(const std::filesystem::directory_entry& Entry,
			std::error_code& Error) const -> std::filesystem::file_status;
		auto RefreshAssetDirectoryIndex(std::shared_ptr<const FAssetCatalogSnapshot> Catalog,
			const FTaskCancellationToken& Cancellation) -> void;
		auto AppendAssetItem(const FPackagePath& Path, const FAssetData& Data,
			const FTaskCancellationToken& Cancellation) -> void;
		auto AddEnumerationDiagnostic(EEnumerationDiagnosticKind Kind,
			const std::filesystem::path& Path, std::string Message) -> void;

		uint64 Version = 0;
		FContentBrowserItemsSnapshot Result;
		FEntryStatusQuery EntryStatusQuery;
		std::shared_ptr<const FAssetCatalogSnapshot> AssetCatalogSnapshot;
		// Pointers refer exclusively to the owned catalog, rebuilt on revision changes.
		std::unordered_map<std::string, std::vector<FIndexedAsset>> AssetDirectoryIndex;
	};
}
