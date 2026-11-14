#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItemData.h"
#include "ContentBrowserOperationContext.h"
#include "Misc/ContentFilesystem.h"
namespace Durin::ContentBrowser::FileDataSource::Private
{
	using FContentBrowserNativeItemLocation = ContentStorage::Filesystem::FLocation;
	struct FContentBrowserFolderItemDataPayload final : IContentBrowserItemDataPayload
	{
		FContentBrowserNativeItemLocation Location;
		std::shared_ptr<const FContentBrowserOperationContext> Host;
	};
	struct FContentBrowserFileItemDataPayload final : IContentBrowserItemDataPayload
	{
		FContentBrowserNativeItemLocation Location;
		std::string Extension;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		std::shared_ptr<const FContentBrowserOperationContext> Host;
	};
}
