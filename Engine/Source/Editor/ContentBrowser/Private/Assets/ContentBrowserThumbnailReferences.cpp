#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Panels/ContentBrowserChanges.h"
#include "Panels/ContentBrowserDataTypes.h"
#include "Assets/SourceImageThumbnailDecoder.h"

#include "Assets/SourceImageThumbnailCache.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin::ContentBrowser::Private
{
	FContentBrowserThumbnailReferences::FContentBrowserThumbnailReferences()
		: SourceImages(std::make_unique<FSourceImageThumbnailCache>())
	{
	}



	FContentBrowserThumbnailReferences::~FContentBrowserThumbnailReferences()
	{
		Clear();
	}

	auto FContentBrowserThumbnailReferences::BeginFrame() -> void
	{
		RequestedItems.clear();
		SourceImages->BeginFrame();
	}

	auto FContentBrowserThumbnailReferences::Request(
		const FContentBrowserItem& Item, EAssetThumbnailPriority Priority) -> void
	{
		const auto& Identity = Item.GetItemKey();
		if (!Identity.IsValid() || Item.IsFolder()) return;
		RequestedItems.insert(Identity);
		if ((!Item.IsFolder() && !Item.IsAsset()))
		{
			FContentBrowserItemDataAttributeValue Extension, Size, Time;
			std::string Physical;
			if (!Item.GetItemAttribute(ContentBrowserItemAttributes::Extension, Extension)
				|| !Extension.GetValue<std::string>() || !IsSupportedSourceImageExtension(*Extension.GetValue<std::string>())
				|| !Item.GetItemPhysicalPath(Physical)
				|| !Item.GetItemAttribute(ContentBrowserItemAttributes::ItemDiskSize, Size)
				|| !Item.GetItemAttribute(ContentBrowserItemAttributes::LastWriteTime, Time) || !Time.GetValue<int64>()) return;
			const auto* UnsignedSize=Size.GetValue<uint64>();
			const auto* SignedSize=Size.GetValue<int64>();
			if (!UnsignedSize && (!SignedSize || *SignedSize<0)) return;
			const auto Bytes=UnsignedSize ? *UnsignedSize : static_cast<uint64>(*SignedSize);
			SourcePaths[Identity] = Physical;
			SourceImages->Request({
				.Identity = Physical, .PhysicalPath = std::move(Physical),
				.FileSize = Bytes,
				.LastWriteTime = std::filesystem::file_time_type(std::filesystem::file_time_type::duration(*Time.GetValue<int64>())),
				.Priority = Priority});
			return;
		}
		FTopLevelAssetPath Path;
		FContentBrowserItemDataAttributeValue Resource;
		if (!Item.GetItemAttribute(ContentBrowserItemAttributes::AssetPath, Resource) || !Resource.GetValue<std::string>()
			|| !FTopLevelAssetPath::TryCreate(*Resource.GetValue<std::string>(), Path))
		{
			AssetThumbnails.erase(Identity); return;
		}
		const auto Asset = FindAssetExact(Path);
		if (!Asset || !GetDefaultThumbnailManager().Find(Asset->AssetClassName))
		{
			AssetThumbnails.erase(Identity);
			return;
		}
		auto& Thumbnail = AssetThumbnails[Identity];
		if (!Thumbnail) Thumbnail = std::make_unique<FAssetThumbnail>(*Asset.Asset);
		else Thumbnail->Reassign(*Asset.Asset);
		Thumbnail->AccessTexture(Priority);
	}

	auto FContentBrowserThumbnailReferences::Find(const FContentBrowserItemKey& Identity) const
		-> FAssetThumbnailView
	{
		const auto It = AssetThumbnails.find(Identity);
		return It == AssetThumbnails.end()
			? (SourcePaths.contains(Identity) ? SourceImages->Find(SourcePaths.at(Identity)) : FAssetThumbnailView{})
			: It->second->GetView();
	}

	auto FContentBrowserThumbnailReferences::EndFrame() -> void
	{
		// Only visible cards and the bounded prefetch range pin shared resources.
		std::erase_if(AssetThumbnails, [&](const auto& Entry) { return !RequestedItems.contains(Entry.first); });
		std::erase_if(SourcePaths, [&](const auto& Entry) { return !RequestedItems.contains(Entry.first); });
		SourceImages->EndFrame();
	}

	auto FContentBrowserThumbnailReferences::CancelPendingRequests() -> void
	{
		SourceImages->CancelPendingRequests();
		// Releasing this panel's references cannot cancel another panel's entries.
		AssetThumbnails.clear();
		SourcePaths.clear();
		RequestedItems.clear();
	}

	auto FContentBrowserThumbnailReferences::ApplyContentChanges(const FContentChangeBatch& Changes) -> void
	{
		std::erase_if(SourcePaths, [&](const auto& Entry) {
			const bool Affected = Changes.bFullRefresh || std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
				return ContentBrowserChanges::SamePath(Entry.second, Change.OldPhysicalPath)
					|| ContentBrowserChanges::SamePath(Entry.second, Change.NewPhysicalPath)
					|| (Change.bDirectory && ContentBrowserChanges::Within(Entry.second, Change.OldPhysicalPath));
			});
			if (Affected) SourceImages->Invalidate(Entry.second);
			return Affected;
		});
		if (!AssetThumbnails.empty())
			GetDefaultThumbnailManager().GetSharedPool().ApplyContentChanges(Changes);
	}

	auto FContentBrowserThumbnailReferences::Clear() -> void
	{
		SourceImages->Clear();
		SourcePaths.clear();
		AssetThumbnails.clear();
		RequestedItems.clear();
	}
} // namespace Durin::ContentBrowser::Private
