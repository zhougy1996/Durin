#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItemData.h"
#include "ContentBrowserOperationContext.h"
#include "DObject/AssetPath.h"
#include "Misc/ContentFilesystem.h"
namespace Durin::ContentBrowser::AssetDataSource::Private
{
	using FContentBrowserNativeItemLocation = ContentStorage::Filesystem::FLocation;
	struct FContentBrowserAssetFolderItemDataPayload final : IContentBrowserItemDataPayload
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
}
