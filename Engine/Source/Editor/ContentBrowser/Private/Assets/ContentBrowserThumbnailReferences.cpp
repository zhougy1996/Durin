#include "Assets/ContentBrowserThumbnailReferences.h"

#include "Assets/SourceImageThumbnailCache.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin::Editor::ContentBrowser::Private
{
	FContentBrowserThumbnailReferences::FContentBrowserThumbnailReferences()
		: SourceImages(std::make_unique<FSourceImageThumbnailCache>())
	{
	}

	FContentBrowserThumbnailReferences::FContentBrowserThumbnailReferences(
		FTaskScopeToken ThumbnailTaskScope)
		: SourceImages(std::make_unique<FSourceImageThumbnailCache>(
			std::move(ThumbnailTaskScope)))
	{
	}

	FContentBrowserThumbnailReferences::~FContentBrowserThumbnailReferences()
	{
		Clear();
	}

	auto FContentBrowserThumbnailReferences::BeginFrame() -> void
	{
		SourceImages->BeginFrame();
		::Durin::Editor::GetDefaultThumbnailManager().GetSharedPool().BeginFrame();
	}

	auto FContentBrowserThumbnailReferences::Request(
		const FContentBrowserThumbnailRequest& Request) -> void
	{
		if (Request.Identity.empty()) return;
		if (!Request.SourcePhysicalPath.empty())
		{
			AssetThumbnails.erase(std::string(Request.Identity));
			SourceImages->Request({
				.Identity = Request.Identity,
				.PhysicalPath = Request.SourcePhysicalPath,
				.FileSize = Request.SourceFileSize,
				.LastWriteTime = Request.SourceLastWriteTime,
				.Priority = Request.Priority});
			return;
		}
		if (!Request.Asset) return;
		auto& Thumbnail = AssetThumbnails[std::string(Request.Identity)];
		if (!Thumbnail)
			Thumbnail = std::make_unique<::Durin::Editor::FAssetThumbnail>(*Request.Asset);
		else
			Thumbnail->Reassign(*Request.Asset);
		Thumbnail->Request(Request.Priority);
	}

	auto FContentBrowserThumbnailReferences::Find(std::string_view Identity) const
		-> ::Durin::Editor::FAssetThumbnailView
	{
		const auto It = AssetThumbnails.find(std::string(Identity));
		return It == AssetThumbnails.end()
			? SourceImages->Find(Identity)
			: It->second->GetView();
	}

	auto FContentBrowserThumbnailReferences::EndFrame() -> void
	{
		SourceImages->EndFrame();
		::Durin::Editor::GetDefaultThumbnailManager().GetSharedPool().EndFrame();
	}

	auto FContentBrowserThumbnailReferences::CancelPendingRequests() -> void
	{
		SourceImages->CancelPendingRequests();
		// Releasing this panel's references cannot cancel another panel's entries.
		AssetThumbnails.clear();
	}

	auto FContentBrowserThumbnailReferences::Clear() -> void
	{
		SourceImages->Clear();
		AssetThumbnails.clear();
	}
} // namespace Durin::Editor::ContentBrowser::Private
