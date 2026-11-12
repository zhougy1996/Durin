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
		SourceImages->BeginFrame();
	}

	auto FContentBrowserThumbnailReferences::Request(
		const FContentBrowserItem& Item, EAssetThumbnailPriority Priority) -> void
	{
		const auto& Identity = Item.GetItemKey();
		if (!Identity.IsValid() || Item.IsFolder()) return;
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
			AssetThumbnails.erase(Identity); Dependencies.erase(Identity); return;
		}
		const auto Asset = FindAssetExact(Path);
		if (!Asset || !GetDefaultThumbnailManager().Find(Asset->AssetClassName))
		{
			AssetThumbnails.erase(Identity);
			Dependencies.erase(Identity);
			return;
		}
		const auto Package = FindPackageExact(Path.GetPackagePath());
		const FAssetThumbnailPackageFingerprint Fingerprint{
			.AssetPath = Path, .PackagePath = Path.GetPackagePath(), .AssetClassName = Asset->AssetClassName,
			.PackageFormatVersion = Package ? Package->FormatVersion : 0,
			.FileSize = Package ? static_cast<uint64>(Package->FileSize) : 0,
			.LastWriteTimeTicks = Package ? Package->LastWriteTimeTicks : 0};
		auto& Thumbnail = AssetThumbnails[Identity];
		if (Thumbnail && Thumbnail->GetAssetPath() != Path) Dependencies.erase(Identity);
		if (!Dependencies.contains(Identity))
			Dependencies.emplace(Identity, CaptureAssetDependencyClosure(Fingerprint.PackagePath));
		if (!Thumbnail) Thumbnail = std::make_unique<FAssetThumbnail>(Fingerprint);
		else Thumbnail->Reassign(Fingerprint);
		Thumbnail->Request(Priority);
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
		SourceImages->EndFrame();
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
			if (Affected) SourceImages->Invalidate(Entry.second);
			return Affected;
		});
		std::erase_if(AssetThumbnails, [&](auto& Entry) {
			const auto It = Dependencies.find(Entry.first);
			const bool Affected = Changes.bFullRefresh || It == Dependencies.end() || !It->second
				|| std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
					if (ContentBrowserChanges::MatchesAsset(Entry.second->GetAssetPath().ToString(), Change.OldAssetPath)
						|| ContentBrowserChanges::MatchesAsset(Entry.second->GetAssetPath().ToString(), Change.NewAssetPath)) return true;
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
} // namespace Durin::ContentBrowser::Private
