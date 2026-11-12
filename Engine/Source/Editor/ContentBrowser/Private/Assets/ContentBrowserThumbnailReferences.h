#pragma once

#include "CoreMinimal.h"

#include "Thumbnail/AssetThumbnail.h"
#include "AssetRegistry/Catalog.h"
#include "ContentBrowserItem.h"

namespace Durin::ContentBrowser::Private
{
	class FSourceImageThumbnailCache;


	// Keeps ordinary-file previews private while asset entries are represented by
	// lightweight FAssetThumbnail references into the manager-owned shared pool.
	class FContentBrowserThumbnailReferences
	{
	public:
		// Test/local caches may use the source cache's local task scope.
		FContentBrowserThumbnailReferences();
		~FContentBrowserThumbnailReferences();

		FContentBrowserThumbnailReferences(const FContentBrowserThumbnailReferences&) = delete;
		FContentBrowserThumbnailReferences& operator=(const FContentBrowserThumbnailReferences&) = delete;

		auto BeginFrame() -> void;
		auto Request(const FContentBrowserItem& Item, EAssetThumbnailPriority Priority) -> void;
		auto Find(const FContentBrowserItemKey& Identity) const -> FAssetThumbnailView;
		auto EndFrame() -> void;
		auto CancelPendingRequests() -> void;
		auto Clear() -> void;
		auto ApplyContentChanges(const FContentChangeBatch& Changes) -> void;
		auto GetAssetReferenceCountForTesting() const -> size_t { return AssetThumbnails.size(); }

	private:
		std::unique_ptr<FSourceImageThumbnailCache> SourceImages;
		std::unordered_map<FContentBrowserItemKey, FAssetDependencyClosureSnapshot, FContentBrowserItemKeyHash> Dependencies;
		std::unordered_map<FContentBrowserItemKey, std::string, FContentBrowserItemKeyHash> SourcePaths;
		std::unordered_map<FContentBrowserItemKey, std::unique_ptr<FAssetThumbnail>, FContentBrowserItemKeyHash> AssetThumbnails;
	};
} // namespace Durin::ContentBrowser::Private
