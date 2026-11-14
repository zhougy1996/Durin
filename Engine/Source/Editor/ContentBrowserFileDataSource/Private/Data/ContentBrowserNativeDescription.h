#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserFileDataPayload.h"
namespace Durin::ContentBrowser::FileDataSource::Private
{
	inline auto MakeNativeItemDescription(const FContentBrowserNativeItem& Native, std::shared_ptr<const FContentBrowserOperationContext> Host = {}) -> FContentBrowserItemData
	{
		std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
		if (Native.Kind == EContentBrowserItemKind::Folder)
		{
			auto P = std::make_shared<FContentBrowserFolderItemDataPayload>(); P->Location.PhysicalPath = Native.PhysicalPath; P->Host = std::move(Host); Payload = std::move(P);
		}
		else if (Native.Kind == EContentBrowserItemKind::File)
		{
			auto P=std::make_shared<FContentBrowserFileItemDataPayload>(); P->Location.PhysicalPath=Native.PhysicalPath; P->Extension=Native.Extension; P->FileSize=Native.FileSize; P->LastWriteTime=Native.LastWriteTime; P->Host=std::move(Host); Payload=std::move(P);
		}
		else return {};
		return {(Native.Kind == EContentBrowserItemKind::Folder ? EContentBrowserItemFlags::Type_Folder : EContentBrowserItemFlags::Type_File) | EContentBrowserItemFlags::Category_Misc, Native.VirtualPath, Native.Name, {}, std::move(Payload)};
	}
}
