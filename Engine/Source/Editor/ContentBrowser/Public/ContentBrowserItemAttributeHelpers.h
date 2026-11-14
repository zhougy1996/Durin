#pragma once
#include "ContentBrowserItem.h"
#include "DObject/AssetPath.h"

namespace Durin::ContentBrowser::Private::ItemAttributes
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
