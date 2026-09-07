#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Panels/ContentBrowserChanges.h"

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
			SourcePaths[std::string(Request.Identity)] = Request.SourcePhysicalPath;
			SourceImages->Request({
				.Identity = Request.Identity,
				.PhysicalPath = Request.SourcePhysicalPath,
				.FileSize = Request.SourceFileSize,
				.LastWriteTime = Request.SourceLastWriteTime,
				.Priority = Request.Priority});
			return;
		}
		if (!Request.Asset) return;
		if (!Dependencies.contains(std::string(Request.Identity)))
			Dependencies.emplace(std::string(Request.Identity), CaptureAssetDependencyClosure(Request.Asset->AssetPath.GetPackagePath()));
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
		Dependencies.clear();
	}

	auto FContentBrowserThumbnailReferences::ApplyContentChanges(const FContentChangeBatch& Changes) -> void
	{
		std::erase_if(SourcePaths, [&](const auto& Entry) {
			const bool Affected = Changes.bFullRefresh || std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
				return ContentBrowserChanges::SamePath(Entry.second, Change.OldPhysicalPath)
					|| ContentBrowserChanges::SamePath(Entry.second, Change.NewPhysicalPath)
					|| (Change.bDirectory && ContentBrowserChanges::Within(Entry.second, Change.OldPhysicalPath));
			});
			if (Affected) SourceImages->Invalidate(Entry.first);
			return Affected;
		});
		std::erase_if(AssetThumbnails, [&](auto& Entry) {
			const auto It = Dependencies.find(Entry.first);
			const bool Affected = Changes.bFullRefresh || It == Dependencies.end() || !It->second
				|| std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
					if (ContentBrowserChanges::MatchesAsset(Entry.first, Change.OldAssetPath)
						|| ContentBrowserChanges::MatchesAsset(Entry.first, Change.NewAssetPath)) return true;
					return std::ranges::any_of(It->second.Assets, [&](const auto& Asset) {
						return ContentBrowserChanges::SamePath(Asset.PhysicalPath, Change.OldPhysicalPath)
							|| ContentBrowserChanges::SamePath(Asset.PhysicalPath, Change.NewPhysicalPath)
							|| (Change.bDirectory && ContentBrowserChanges::Within(Asset.PhysicalPath, Change.OldPhysicalPath));
					});
				});
			if (Affected) { Entry.second->Refresh(); Dependencies.erase(Entry.first); }
			return Affected;
		});
	}

	auto FContentBrowserThumbnailReferences::Clear() -> void
	{
		SourceImages->Clear();
		SourcePaths.clear();
		AssetThumbnails.clear();
		Dependencies.clear();
	}
} // namespace Durin::Editor::ContentBrowser::Private
