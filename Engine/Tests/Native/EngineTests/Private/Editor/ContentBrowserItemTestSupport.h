#pragma once
#include "Editor/ContentBrowserStorageFixture.h"


namespace Durin::ContentBrowser::Private::FixtureAttributes
{
	inline auto String(const FContentBrowserItem& Item, std::string_view Key) -> std::string
	{
		FContentBrowserItemDataAttributeValue Value;
		if (Item.GetItemAttribute(Key, Value))
			if (const auto* Text = Value.GetValue<std::string>()) return *Text;
		return {};
	}
	inline auto Unsigned(const FContentBrowserItem& Item, std::string_view Key) -> uint64
	{
		FContentBrowserItemDataAttributeValue Value;
		if (Item.GetItemAttribute(Key, Value))
		{
			if (const auto* Number = Value.GetValue<uint64>()) return *Number;
			if (const auto* Number = Value.GetValue<int64>(); Number && *Number >= 0) return static_cast<uint64>(*Number);
		}
		return 0;
	}
	inline auto Signed(const FContentBrowserItem& Item, std::string_view Key) -> int64
	{
		FContentBrowserItemDataAttributeValue Value;
		if (Item.GetItemAttribute(Key, Value))
			if (const auto* Number = Value.GetValue<int64>()) return *Number;
		return 0;
	}
	inline auto PhysicalPath(const FContentBrowserItem& Item) -> std::string
	{
		std::string Path;
		Item.GetItemPhysicalPath(Path);
		return Path;
	}
	inline auto PackagePath(const FContentBrowserItem& Item) -> FPackagePath
	{
		FPackagePath Path;
		FPackagePath::TryCreate(String(Item, ContentBrowserItemAttributes::PackagePath), Path);
		return Path;
	}
	inline auto RedirectDestination(const FContentBrowserItem& Item) -> FSoftObjectPath
	{
		FSoftObjectPath Path;
		FSoftObjectPath::TryCreate(String(Item, ContentBrowserItemAttributes::RedirectDestination), Path);
		return Path;
	}
	inline auto LastWriteTime(const FContentBrowserItem& Item) -> std::filesystem::file_time_type
	{
		return std::filesystem::file_time_type(std::filesystem::file_time_type::duration(Signed(Item, ContentBrowserItemAttributes::LastWriteTime)));
	}
	inline auto RenameName(const FContentBrowserItem& Item) -> std::string
	{
		const auto Package = PackagePath(Item);
		return Package.IsValid() ? std::string(Package.GetPackageName()) : Item.GetItemName();
	}
}

namespace Durin::ContentBrowser::Private
{
	inline auto ItemDescriptions(std::initializer_list<FStorageItemFixture> Items) -> std::vector<FContentBrowserItem>
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
	template<typename T> auto TestPhysicalPath(const T& Item) { if constexpr (requires { Item.PhysicalPath; }) return Item.PhysicalPath; else return FixtureAttributes::PhysicalPath(Item); }
	template<typename T> auto TestPackagePath(const T& Item) { if constexpr (requires { Item.PackagePath; }) return Item.PackagePath; else return FixtureAttributes::PackagePath(Item); }
	template<typename T> auto TestAssetClassName(const T& Item) { if constexpr (requires { Item.AssetClassName; }) return Item.AssetClassName; else return FixtureAttributes::String(Item, ContentBrowserItemAttributes::Class); }
	template<typename T> auto TestExtension(const T& Item) { if constexpr (requires { Item.Extension; }) return Item.Extension; else return FixtureAttributes::String(Item, ContentBrowserItemAttributes::Extension); }
	template<typename T> auto TestRedirectDestination(const T& Item) { if constexpr (requires { Item.RedirectDestination; }) return Item.RedirectDestination; else return FixtureAttributes::RedirectDestination(Item); }
	template<typename T> auto TestFileSize(const T& Item) { if constexpr (requires { Item.FileSize; }) return Item.FileSize; else return FixtureAttributes::Unsigned(Item, ContentBrowserItemAttributes::ItemDiskSize); }
	template<typename T> auto TestPackageFileSize(const T& Item) { if constexpr (requires { Item.PackageFileSize; }) return Item.PackageFileSize; else return FixtureAttributes::Unsigned(Item, ContentBrowserItemAttributes::PackageFileSize); }
	template<typename T> auto TestBulkFileSize(const T& Item) { if constexpr (requires { Item.BulkFileSize; }) return Item.BulkFileSize; else return FixtureAttributes::Unsigned(Item, ContentBrowserItemAttributes::BulkFileSize); }
	template<typename T> auto TestLastWriteTime(const T& Item) { if constexpr (requires { Item.LastWriteTime; }) return Item.LastWriteTime; else return FixtureAttributes::LastWriteTime(Item); }
	template<typename T> auto TestName(const T& Item) { if constexpr (requires { Item.Name; }) return Item.Name; else return Item.GetItemName(); }
	template<typename T> auto TestVirtualPath(const T& Item) { if constexpr (requires { Item.VirtualPath; }) return Item.VirtualPath; else return Item.GetVirtualPath(); }
}
