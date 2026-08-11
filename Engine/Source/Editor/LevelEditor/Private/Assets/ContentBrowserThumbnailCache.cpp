#include "Assets/ContentBrowserThumbnailCache.h"

#include "Assets/SourceImageThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"

namespace Durin
{
	FContentBrowserThumbnailCache::FContentBrowserThumbnailCache()
		: SourceImages(std::make_unique<FSourceImageThumbnailCache>())
		, RenderedAssets(std::make_unique<Editor::FRenderedAssetThumbnailCache>())
	{
	}

	FContentBrowserThumbnailCache::~FContentBrowserThumbnailCache()
	{
		Clear();
	}

	auto FContentBrowserThumbnailCache::BeginFrame() -> void
	{
		SourceImages->BeginFrame();
		RenderedAssets->BeginFrame();
	}

	auto FContentBrowserThumbnailCache::Request(
		const FContentBrowserThumbnailRequest& Request) -> void
	{
		if (Request.Identity.empty()) return;
		if (!Request.SourcePhysicalPath.empty())
		{
			RenderedIdentities.erase(std::string(Request.Identity));
			SourceImages->Request({
				.Identity = Request.Identity,
				.PhysicalPath = Request.SourcePhysicalPath,
				.FileSize = Request.SourceFileSize,
				.LastWriteTime = Request.SourceLastWriteTime,
				.Priority = Request.Priority});
			return;
		}
		if (!Request.Asset) return;
		RenderedIdentities.insert_or_assign(
			std::string(Request.Identity), Request.Asset->VirtualPath);
		RenderedAssets->Request(*Request.Asset, Request.Priority);
	}

	auto FContentBrowserThumbnailCache::Find(std::string_view Identity) const
		-> Editor::FAssetThumbnailView
	{
		const auto It = RenderedIdentities.find(std::string(Identity));
		return It == RenderedIdentities.end()
			? SourceImages->Find(Identity)
			: RenderedAssets->Find(It->second);
	}

	auto FContentBrowserThumbnailCache::EndFrame() -> void
	{
		SourceImages->EndFrame();
		RenderedAssets->EndFrame();
	}

	auto FContentBrowserThumbnailCache::CancelPendingRequests() -> void
	{
		SourceImages->CancelPendingRequests();
		RenderedAssets->CancelPendingRequests();
	}

	auto FContentBrowserThumbnailCache::Clear() -> void
	{
		SourceImages->Clear();
		RenderedAssets->Clear();
		RenderedIdentities.clear();
	}
} // namespace Durin
