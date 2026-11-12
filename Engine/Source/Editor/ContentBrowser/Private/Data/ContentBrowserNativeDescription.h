#pragma once
#include "Data/ContentBrowserNativeItem.h"
#include "Data/ContentBrowserNativeItemPayloads.h"

namespace Durin::ContentBrowser::Private
{
	inline auto MakeNativeItemDescription(const FContentBrowserNativeItem& Item, bool AssetSource, std::shared_ptr<const FContentBrowserOperationContext> Host = {}) -> FContentBrowserItemData
		{
			using namespace ContentBrowser::Private;
			auto Native=Item;
			const FContentBrowserNativeItemLocation Location{Native.PhysicalPath};
			std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
			if (Native.Kind==EContentBrowserItemKind::Asset || Native.Kind==EContentBrowserItemKind::Redirector)
			{
				auto P=std::make_shared<FContentBrowserAssetFileItemDataPayload>();
				P->Location=Location; FTopLevelAssetPath::TryCreate(Native.VirtualPath, P->AssetPath); P->PackagePath=Native.PackagePath; P->AssetClassName=Native.AssetClassName; P->Extension=Native.Extension;
				P->RedirectDestination=Native.RedirectDestination; P->FileSize=Native.FileSize; P->PackageFileSize=Native.PackageFileSize;
				P->BulkFileSize=Native.BulkFileSize; P->LastWriteTime=Native.LastWriteTime; P->bRedirector=Native.Kind==EContentBrowserItemKind::Redirector;
				P->Host=std::move(Host); Payload=std::move(P);
			}
			else if (Native.Kind==EContentBrowserItemKind::File)
			{
				auto P=std::make_shared<FContentBrowserFileItemDataPayload>();
				P->Location=Location; P->Extension=Native.Extension; P->FileSize=Native.FileSize; P->LastWriteTime=Native.LastWriteTime;
				P->Host=std::move(Host); Payload=std::move(P);
			}
			else if (AssetSource)
			{
				auto P=std::make_shared<FContentBrowserAssetFolderItemDataPayload>();
				P->Location=Location; P->Host=std::move(Host); Payload=std::move(P);
			}
			else
			{
				auto P=std::make_shared<FContentBrowserFolderItemDataPayload>();
				P->Location=Location; P->Host=std::move(Host); Payload=std::move(P);
			}
			return {(Native.Kind == EContentBrowserItemKind::Folder ? EContentBrowserItemFlags::Type_Folder : EContentBrowserItemFlags::Type_File)
				| (AssetSource ? EContentBrowserItemFlags::Category_Asset : EContentBrowserItemFlags::Category_Misc),
				Native.VirtualPath, Native.Name, {}, std::move(Payload)};
		}
	inline auto MakeNativeItem(const FContentBrowserNativeItem& Native, bool AssetSource) -> FContentBrowserItem
	{
		return {.InternalItems = {MakeNativeItemDescription(Native, AssetSource)}};
	}
	inline FContentBrowserNativeItem::operator FContentBrowserItem() const
	{
		return MakeNativeItem(*this, Kind != EContentBrowserItemKind::File);
	}

}
