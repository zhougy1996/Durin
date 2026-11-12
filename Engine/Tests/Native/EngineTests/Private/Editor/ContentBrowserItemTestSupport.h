#pragma once
#include "Data/ContentBrowserNativeDescription.h"
#include "Data/ContentBrowserItemAttributeHelpers.h"

namespace Durin::ContentBrowser::Private
{
	inline auto ItemDescriptions(std::initializer_list<FContentBrowserNativeItem> Items) -> std::vector<FContentBrowserItem>
	{
		std::vector<FContentBrowserItem> Result;
		for (auto Item : Items)
		{
			// Synthetic fixtures use explicit slash-rooted test paths as their virtual namespace.
			if (Item.VirtualPath.empty() && Item.PhysicalPath.starts_with('/')) Item.VirtualPath = Item.PhysicalPath;
			if (Item.Extension.empty()) Item.Extension = std::filesystem::path(Item.PhysicalPath).extension().generic_string();
			Result.push_back(Item);
		}
		return Result;
	}
	template<typename T> auto TestKind(const T& Item) requires requires { Item.Kind; } { return Item.Kind; }
	inline auto TestKind(const FContentBrowserItem& Item) -> EContentBrowserItemKind
	{
		return Item.IsFolder() ? EContentBrowserItemKind::Folder : Item.IsRedirector() ? EContentBrowserItemKind::Redirector
			: Item.IsAsset() ? EContentBrowserItemKind::Asset : EContentBrowserItemKind::File;
	}
	template<typename T> auto TestPhysicalPath(const T& Item) { if constexpr (requires { Item.PhysicalPath; }) return Item.PhysicalPath; else return ItemAttributes::PhysicalPath(Item); }
	template<typename T> auto TestPackagePath(const T& Item) { if constexpr (requires { Item.PackagePath; }) return Item.PackagePath; else return ItemAttributes::PackagePath(Item); }
	template<typename T> auto TestAssetClassName(const T& Item) { if constexpr (requires { Item.AssetClassName; }) return Item.AssetClassName; else return ItemAttributes::String(Item, ContentBrowserItemAttributes::Class); }
	template<typename T> auto TestExtension(const T& Item) { if constexpr (requires { Item.Extension; }) return Item.Extension; else return ItemAttributes::String(Item, ContentBrowserItemAttributes::Extension); }
	template<typename T> auto TestRedirectDestination(const T& Item) { if constexpr (requires { Item.RedirectDestination; }) return Item.RedirectDestination; else return ItemAttributes::RedirectDestination(Item); }
	template<typename T> auto TestFileSize(const T& Item) { if constexpr (requires { Item.FileSize; }) return Item.FileSize; else return ItemAttributes::Unsigned(Item, ContentBrowserItemAttributes::ItemDiskSize); }
	template<typename T> auto TestPackageFileSize(const T& Item) { if constexpr (requires { Item.PackageFileSize; }) return Item.PackageFileSize; else return ItemAttributes::Unsigned(Item, ContentBrowserItemAttributes::PackageFileSize); }
	template<typename T> auto TestBulkFileSize(const T& Item) { if constexpr (requires { Item.BulkFileSize; }) return Item.BulkFileSize; else return ItemAttributes::Unsigned(Item, ContentBrowserItemAttributes::BulkFileSize); }
	template<typename T> auto TestLastWriteTime(const T& Item) { if constexpr (requires { Item.LastWriteTime; }) return Item.LastWriteTime; else return ItemAttributes::LastWriteTime(Item); }
	template<typename T> auto TestName(const T& Item) { if constexpr (requires { Item.Name; }) return Item.Name; else return Item.GetItemName(); }
	template<typename T> auto TestVirtualPath(const T& Item) { if constexpr (requires { Item.VirtualPath; }) return Item.VirtualPath; else return Item.GetVirtualPath(); }
}
