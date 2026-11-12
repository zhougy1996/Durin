#pragma once
#include "CoreMinimal.h"
#include "DObject/WeakObjectPtr.h"
#include "ContentBrowserAPI.h"
#include "ContentBrowserItemKey.h"
#include "ContentBrowserItemAttributes.h"

namespace Durin
{
	class DContentBrowserDataSource;
}
namespace Durin::ContentBrowser
{
	class IContentBrowserDataCompiledFilter;
	// Metadata category, independent of the provider that owns an item.
	enum class EContentBrowserItemKind : uint8
	{
		Folder,
		Asset,
		Redirector,
		File
	};
	// UE save predicates; unspecified flags permit loading and saving clean packages.
	enum class EContentBrowserItemSaveFlags : uint8
	{
		None = 0,
		SaveOnlyIfDirty = 1,
		SaveOnlyIfLoaded = 2
	};
	constexpr auto operator|(EContentBrowserItemSaveFlags A, EContentBrowserItemSaveFlags B) -> EContentBrowserItemSaveFlags
	{
		return static_cast<EContentBrowserItemSaveFlags>(static_cast<uint8>(A) | static_cast<uint8>(B));
	}
	// Source-defined immutable data; the browser never interprets its concrete type.
	class IContentBrowserItemDataPayload
	{
	public:
		virtual ~IContentBrowserItemDataPayload() = default;
	};
	// One source's item identity. Reactivation never revives an earlier handle.
	class FContentBrowserItemData
	{
	public:
		CONTENTBROWSER_API FContentBrowserItemData();
		CONTENTBROWSER_API FContentBrowserItemData(EContentBrowserItemFlags Flags, std::string VirtualPath, std::string Name, std::string DisplayName, std::shared_ptr<const IContentBrowserItemDataPayload> Payload);
		CONTENTBROWSER_API FContentBrowserItemData(DContentBrowserDataSource& Owner, EContentBrowserItemFlags Flags, std::string VirtualPath, std::string Name, std::string DisplayName, std::shared_ptr<const IContentBrowserItemDataPayload> Payload);
		CONTENTBROWSER_API auto GetOwnerDataSource() const -> DContentBrowserDataSource*;
		auto GetVirtualPath() const -> const std::string& { return VirtualPath; }
		auto GetItemType() const -> EContentBrowserItemFlags { return static_cast<EContentBrowserItemFlags>(static_cast<uint16>(Flags) & static_cast<uint16>(EContentBrowserItemFlags::Type_MASK)); }
		auto GetItemFlags() const -> EContentBrowserItemFlags { return Flags; }
		auto GetItemName() const -> const std::string& { return Name; }
		auto GetDisplayName() const -> const std::string& { return DisplayName; }
		auto GetOwnerKey() const -> FObjectKey { return Owner.GetKey(); }
		auto GetItemDataKey() const -> FContentBrowserItemDataKey { return FContentBrowserItemDataKey(GetItemType(), VirtualPath); }
		auto GetItemKey() const -> FContentBrowserItemKey { return {GetItemType(), VirtualPath, Owner.GetKey()}; }
		auto GetContributionKey() const -> FContentBrowserItemContributionKey { return {GetItemDataKey(), Owner.GetKey(), Generation, MountRevision}; }
		auto GetPayload() const -> const std::shared_ptr<const IContentBrowserItemDataPayload>& { return Payload; }
		CONTENTBROWSER_API auto GetItemAttribute(std::string_view Key, FContentBrowserItemDataAttributeValue& Out, bool IncludeMetaData = false) const -> bool;
		CONTENTBROWSER_API auto GetItemAttributes(FContentBrowserItemDataAttributeValues& Out, bool IncludeMetaData = false) const -> bool;
		CONTENTBROWSER_API auto GetItemPhysicalPath(std::string& Out) const -> bool;

	private:
		TWeakObjectPtr<DContentBrowserDataSource> Owner;
		uint64 Generation = 0;
		uint64 MountRevision = 0;
		EContentBrowserItemFlags Flags = EContentBrowserItemFlags::None;
		std::string VirtualPath;
		std::string Name, DisplayName;
		std::shared_ptr<const IContentBrowserItemDataPayload> Payload;
	};
	enum class EContentBrowserItemUpdateType : uint8
	{
		Added,
		Modified,
		Moved,
		Removed
	};
	// A committed source-owned delta. Folder modifications invalidate that logical subtree.
	class FContentBrowserItemDataUpdate
	{
	public:
		static auto MakeItemAddedUpdate(FContentBrowserItemData Item) -> FContentBrowserItemDataUpdate { return {EContentBrowserItemUpdateType::Added, std::move(Item), {}}; }
		static auto MakeItemModifiedUpdate(FContentBrowserItemData Item) -> FContentBrowserItemDataUpdate { return {EContentBrowserItemUpdateType::Modified, std::move(Item), {}}; }
		static auto MakeItemMovedUpdate(FContentBrowserItemData Item, std::string PreviousPath) -> FContentBrowserItemDataUpdate { return {EContentBrowserItemUpdateType::Moved, std::move(Item), std::move(PreviousPath)}; }
		static auto MakeItemRemovedUpdate(FContentBrowserItemData Item) -> FContentBrowserItemDataUpdate { return {EContentBrowserItemUpdateType::Removed, std::move(Item), {}}; }
		auto GetUpdateType() const -> EContentBrowserItemUpdateType { return Type; }
		auto GetItemData() const -> const FContentBrowserItemData& { return Item; }
		auto GetPreviousVirtualPath() const -> const std::string& { return PreviousPath; }

	private:
		FContentBrowserItemDataUpdate(EContentBrowserItemUpdateType InType, FContentBrowserItemData InItem, std::string Previous)
			: Type(InType)
			, Item(std::move(InItem))
			, PreviousPath(std::move(Previous))
		{
		}
		EContentBrowserItemUpdateType Type;
		FContentBrowserItemData Item;
		std::string PreviousPath;
	};
	// Value-owned selection criteria; compiling never captures a live provider.
	class IContentBrowserDataFilterExtra
	{
	public:
		virtual ~IContentBrowserDataFilterExtra() = default;
	};
	struct FContentBrowserDataFilter
	{
		bool bRecursive = false;
		std::optional<EContentBrowserItemKind> Kind;
		std::shared_ptr<const IContentBrowserDataFilterExtra> ExtraFilter;
	};
	// Provider query input, reusable without reviving previously returned item handles.
	struct FContentBrowserDataCompiledFilter
	{
		std::string Path;
		FContentBrowserDataFilter Filter;
		struct FSourceFilter
		{
			uint64 Generation = 0, MountRevision = 0, DiscoveryRevision = 0;
			std::shared_ptr<const IContentBrowserDataCompiledFilter> Data;
		};
		std::map<const DContentBrowserDataSource*, FSourceFilter> CompiledFilters;
	};
} // namespace Durin::ContentBrowser
