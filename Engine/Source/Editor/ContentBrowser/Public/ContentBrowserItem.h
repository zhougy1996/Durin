#pragma once

#include "CoreMinimal.h"
#include "ContentBrowserItemData.h"
#include "ContentBrowserOperationResult.h"

namespace Durin::ContentBrowser
{
	// Presentation category; operation ownership belongs to the item's data source.

	// An aggregate of source descriptions and opaque payloads.
	struct FContentBrowserItem
	{
		std::vector<FContentBrowserItemData> InternalItems;
		auto GetInternalItems() const -> std::span<const FContentBrowserItemData> { return InternalItems; }
		auto GetPrimaryInternalItem() const -> const FContentBrowserItemData* { return InternalItems.empty() ? nullptr : &InternalItems.front(); }
		auto GetItemName() const -> const std::string& { return InternalItems.empty() ? EmptyDescription() : InternalItems.front().GetItemName(); }
		auto GetDisplayName() const -> const std::string& { return InternalItems.empty() ? EmptyDescription() : InternalItems.front().GetDisplayName(); }
		auto GetVirtualPath() const -> const std::string& { return InternalItems.empty() ? EmptyDescription() : InternalItems.front().GetVirtualPath(); }
		auto GetItemFlags() const -> EContentBrowserItemFlags
		{
			auto Flags = EContentBrowserItemFlags::None;
			for (const auto& Data : InternalItems) Flags = Flags | Data.GetItemFlags();
			return Flags;
		}
		auto IsFolder() const -> bool { return (static_cast<uint16>(GetItemFlags()) & static_cast<uint16>(EContentBrowserItemFlags::Type_Folder)) != 0; }
		auto IsAsset() const -> bool { return (static_cast<uint16>(GetItemFlags()) & static_cast<uint16>(EContentBrowserItemFlags::Category_Asset)) != 0; }
		auto IsRedirector() const -> bool
		{
			FContentBrowserItemDataAttributeValue Value;
			return GetItemAttribute(ContentBrowserItemAttributes::IsRedirector, Value) && Value.GetValue<bool>() && *Value.GetValue<bool>();
		}
		static auto EmptyDescription() -> const std::string& { static const std::string Empty; return Empty; }
		CONTENTBROWSER_API auto GetItemAttribute(std::string_view Key, FContentBrowserItemDataAttributeValue& Out, bool IncludeMetaData = false) const -> bool;
		CONTENTBROWSER_API auto GetItemAttributes(FContentBrowserItemDataAttributeValues& Out, bool IncludeMetaData = false) const -> bool;
		CONTENTBROWSER_API auto GetItemPhysicalPath(std::string& Out) const -> bool;
		auto GetItemKey() const -> FContentBrowserItemKey { return InternalItems.empty() ? FContentBrowserItemKey{} : InternalItems.front().GetItemKey(); }
		CONTENTBROWSER_API auto CanEdit() const -> bool;
		CONTENTBROWSER_API auto Edit() const -> bool;
		CONTENTBROWSER_API auto CanDuplicate() const -> bool;
		CONTENTBROWSER_API auto Duplicate(FContentBrowserItem& Out) const -> bool;
		CONTENTBROWSER_API auto CanDelete() const -> bool;
		CONTENTBROWSER_API auto CanSave(EContentBrowserItemSaveFlags Flags) const -> bool;
		CONTENTBROWSER_API auto Save(EContentBrowserItemSaveFlags Flags) const -> FContentBrowserOperationResult;
		CONTENTBROWSER_API auto CanRename(std::string_view Name, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API auto Rename(std::string_view Name, FContentBrowserItem& Out) const -> FContentBrowserOperationResult;
		CONTENTBROWSER_API auto CanCopy(std::string_view Destination, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API auto Copy(std::string_view Destination) const -> FContentBrowserOperationResult;
		CONTENTBROWSER_API auto CanMove(std::string_view Destination, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API auto Move(std::string_view Destination) const -> FContentBrowserOperationResult;

	};
} // namespace Durin::ContentBrowser
