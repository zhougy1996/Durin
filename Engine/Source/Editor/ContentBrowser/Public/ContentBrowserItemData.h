#pragma once
#include "CoreMinimal.h"
#include "DObject/WeakObjectPtr.h"
#include "ContentBrowserAPI.h"

namespace Durin
{
	class DContentBrowserDataSource;
}
namespace Durin::ContentBrowser
{
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
		CONTENTBROWSER_API FContentBrowserItemData(DContentBrowserDataSource& Owner, EContentBrowserItemKind Kind, std::string VirtualPath, std::shared_ptr<const IContentBrowserItemDataPayload> Payload);
		CONTENTBROWSER_API auto GetOwnerDataSource() const -> DContentBrowserDataSource*;
		auto GetVirtualPath() const -> const std::string& { return VirtualPath; }
		auto GetItemKind() const -> EContentBrowserItemKind { return Kind; }
		auto GetPayload() const -> const std::shared_ptr<const IContentBrowserItemDataPayload>& { return Payload; }

	private:
		TWeakObjectPtr<DContentBrowserDataSource> Owner;
		uint64 Generation = 0;
		uint64 MountRevision = 0;
		EContentBrowserItemKind Kind = EContentBrowserItemKind::File;
		std::string VirtualPath;
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
	};
} // namespace Durin::ContentBrowser
