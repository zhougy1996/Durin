#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItemData.h"
#include "ContentBrowserOperationContext.h"
#include "DObject/AssetPath.h"

namespace Durin::ContentBrowser::Private
{
	struct FContentBrowserNativeItemLocation
	{
		std::string PhysicalPath;
	};
	struct FContentBrowserAssetFolderItemDataPayload final : IContentBrowserItemDataPayload
	{
		FContentBrowserNativeItemLocation Location;
		std::shared_ptr<const FContentBrowserOperationContext> Host;
	};
	struct FContentBrowserFolderItemDataPayload final : IContentBrowserItemDataPayload
	{
		FContentBrowserNativeItemLocation Location;
		std::shared_ptr<const FContentBrowserOperationContext> Host;
	};
	struct FContentBrowserAssetFileItemDataPayload final : IContentBrowserItemDataPayload
	{
		FContentBrowserNativeItemLocation Location;
		FTopLevelAssetPath AssetPath;
		FPackagePath PackagePath;
		std::string AssetClassName, Extension;
		FSoftObjectPath RedirectDestination;
		uintmax_t FileSize = 0, PackageFileSize = 0, BulkFileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		bool bRedirector = false;
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
} // namespace Durin::ContentBrowser::Private
