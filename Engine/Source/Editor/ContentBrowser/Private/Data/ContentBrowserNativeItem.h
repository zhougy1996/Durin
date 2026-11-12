#pragma once
#include "ContentBrowserItem.h"
#include "DObject/AssetPath.h"

namespace Durin::ContentBrowser::Private
{
	// Provider-private input to native discovery and storage utilities.
	// Generic items never contain this record.
	struct FContentBrowserNativeItem
	{
		EContentBrowserItemKind Kind = EContentBrowserItemKind::File;
		std::string Name;
		std::string VirtualPath;
		FPackagePath PackagePath;
		std::string PhysicalPath;
		std::string AssetClassName;
		std::string Extension;
		FSoftObjectPath RedirectDestination;
		uintmax_t FileSize = 0;
		uintmax_t PackageFileSize = 0;
		uintmax_t BulkFileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		auto GetItemName() const -> const std::string& { return Name; }
		auto GetVirtualPath() const -> const std::string& { return VirtualPath; }
		operator FContentBrowserItem() const;
	};
}
