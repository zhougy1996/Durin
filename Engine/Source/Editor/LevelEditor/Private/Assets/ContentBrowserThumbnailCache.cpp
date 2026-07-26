#include "Assets/ContentBrowserThumbnailCache.h"

#include "Assets/SourceImageThumbnailCache.h"
#include "Thumbnail/MaterialAssetThumbnail.h"

namespace Durin
{
	FContentBrowserThumbnailCache::FContentBrowserThumbnailCache()
		: SourceImages(std::make_unique<FSourceImageThumbnailCache>())
		, Materials(std::make_unique<FMaterialAssetThumbnailCache>())
	{
	}

	FContentBrowserThumbnailCache::~FContentBrowserThumbnailCache()
	{
		Clear();
	}

	auto FContentBrowserThumbnailCache::BeginFrame() -> void
	{
		SourceImages->BeginFrame();
		Materials->BeginFrame();
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
		Materials->Request(*Request.Asset, Request.Priority);
	}

	auto FContentBrowserThumbnailCache::Find(std::string_view Identity) const
		-> FAssetThumbnailView
	{
		const auto It = RenderedIdentities.find(std::string(Identity));
		return It == RenderedIdentities.end()
			? SourceImages->Find(Identity)
			: Materials->Find(It->second);
	}

	auto FContentBrowserThumbnailCache::EndFrame() -> void
	{
		SourceImages->EndFrame();
		Materials->EndFrame();
	}

	auto FContentBrowserThumbnailCache::CancelPendingRequests() -> void
	{
		SourceImages->CancelPendingRequests();
		Materials->CancelPendingRequests();
	}

	auto FContentBrowserThumbnailCache::Clear() -> void
	{
		SourceImages->Clear();
		Materials->Clear();
		RenderedIdentities.clear();
	}
} // namespace Durin
