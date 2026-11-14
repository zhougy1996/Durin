#pragma once
#include "ContentBrowserItemData.h"

namespace Durin::ContentBrowser::FileDataSource::Private
{
	// Provider-private input to native discovery and storage utilities.
	// Generic items never contain this record.
	struct FContentBrowserNativeItem
	{
		EContentBrowserItemKind Kind = EContentBrowserItemKind::File;
		std::string Name;
		std::string VirtualPath;
		std::string PhysicalPath;
		std::string Extension;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};
		auto GetItemName() const -> const std::string& { return Name; }
		auto GetVirtualPath() const -> const std::string& { return VirtualPath; }
	};
}
