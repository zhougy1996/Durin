#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserAssetDataPayload.h"
namespace Durin::ContentBrowser::AssetDataSource::Private
{
	inline auto MakeNativeItemDescription(const FContentBrowserNativeItem& Native, std::shared_ptr<const FContentBrowserOperationContext> Host = {}) -> FContentBrowserItemData
	{
		std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
		if (Native.Kind == EContentBrowserItemKind::Folder)
		{
			auto P = std::make_shared<FContentBrowserAssetFolderItemDataPayload>(); P->Location.PhysicalPath = Native.PhysicalPath; P->Host = std::move(Host); Payload = std::move(P);
		}
		else if (Native.Kind == EContentBrowserItemKind::Asset || Native.Kind == EContentBrowserItemKind::Redirector)
		{
			auto P = std::make_shared<FContentBrowserAssetFileItemDataPayload>();
			P->Location.PhysicalPath=Native.PhysicalPath; FTopLevelAssetPath::TryCreate(Native.VirtualPath, P->AssetPath); P->PackagePath=Native.PackagePath; P->AssetClassName=Native.AssetClassName; P->Extension=Native.Extension;
			P->RedirectDestination=Native.RedirectDestination; P->FileSize=Native.FileSize; P->PackageFileSize=Native.PackageFileSize; P->BulkFileSize=Native.BulkFileSize; P->LastWriteTime=Native.LastWriteTime; P->bRedirector=Native.Kind==EContentBrowserItemKind::Redirector; P->Host=std::move(Host); Payload=std::move(P);
		}
		else return {};
		return {(Native.Kind == EContentBrowserItemKind::Folder ? EContentBrowserItemFlags::Type_Folder : EContentBrowserItemFlags::Type_File) | EContentBrowserItemFlags::Category_Asset, Native.VirtualPath, Native.Name, {}, std::move(Payload)};
	}
}
