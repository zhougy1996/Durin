#pragma once

#include "CoreMinimal.h"
#include "DObject/AssetPath.h"
#include "ContentBrowserItemData.h"

namespace Durin::ContentBrowser
{
	// Presentation category; operation ownership belongs to the item's data source.

	// Value-owned item snapshot shared by discovery, browser presentation and operations.
	struct FContentBrowserItem
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
		std::vector<FContentBrowserItemData> InternalItems;
		auto GetInternalItems() const -> std::span<const FContentBrowserItemData> { return InternalItems; }
		CONTENTBROWSER_API auto CanEdit() const -> bool;
		CONTENTBROWSER_API auto Edit() const -> bool;
		CONTENTBROWSER_API auto CanDuplicate() const -> bool;
		CONTENTBROWSER_API auto Duplicate(FContentBrowserItem& Out) const -> bool;
		CONTENTBROWSER_API auto CanDelete() const -> bool;
		CONTENTBROWSER_API auto CanSave(EContentBrowserItemSaveFlags Flags) const -> bool;
		CONTENTBROWSER_API auto Save(EContentBrowserItemSaveFlags Flags) const -> bool;
		CONTENTBROWSER_API auto CanRename(std::string_view Name, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API auto Rename(std::string_view Name, FContentBrowserItem& Out) const -> bool;
		CONTENTBROWSER_API auto CanCopy(std::string_view Destination, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API auto Copy(std::string_view Destination) const -> bool;
		CONTENTBROWSER_API auto CanMove(std::string_view Destination, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API auto Move(std::string_view Destination) const -> bool;

		auto StableId() const -> const std::string&
		{
			return Kind == EContentBrowserItemKind::Asset
						   || Kind == EContentBrowserItemKind::Redirector ?
					   VirtualPath :
					   (PhysicalPath.empty() ? VirtualPath : PhysicalPath);
		}
	};
} // namespace Durin::ContentBrowser
