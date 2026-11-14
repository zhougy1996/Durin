#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserAssetDataSourceAPI.h"
#include "ContentBrowserDataSource.h"
#include "DObject/AssetPath.h"
#include "ContentBrowserAssetDataSource.gen.h"
namespace Durin::ContentBrowser::AssetDataSource::Private { struct FContentBrowserAssetSnapshot; struct FContentBrowserFileUpdateState; }
namespace Durin
{
	DCLASS()
	class DContentBrowserAssetDataSource : public DContentBrowserDataSource
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSERASSETDATASOURCE_API explicit DContentBrowserAssetDataSource(const FObjectInitializer&);
		CONTENTBROWSERASSETDATASOURCE_API auto Initialize(bool AutoRegister = true) -> void override;
		CONTENTBROWSERASSETDATASOURCE_API auto Shutdown() -> void override;
		CONTENTBROWSERASSETDATASOURCE_API auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItemData&&)>&) -> void override;
		CONTENTBROWSERASSETDATASOURCE_API auto BindItemData(const ContentBrowser::FContentBrowserItemData&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> ContentBrowser::FContentBrowserItemData override;
		CONTENTBROWSERASSETDATASOURCE_API auto CanCreateFolder(std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CreateFolder(std::string_view, ContentBrowser::FContentBrowserItemDataTemporaryContext&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext override;
		CONTENTBROWSERASSETDATASOURCE_API auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> bool override;

		CONTENTBROWSERASSETDATASOURCE_API auto CanRenameItem(const ContentBrowser::FContentBrowserItemData&, std::optional<std::string_view> = std::nullopt, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CanCopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CanMoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CanEditItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto EditItem(const ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CanDuplicateItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto DuplicateItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto FixUpRedirectors(std::span<const FPackagePath>, const ContentBrowser::FContentBrowserOperationContext&) -> bool;
		CONTENTBROWSERASSETDATASOURCE_API auto FixUpRedirectorsInFolder(std::string_view, const ContentBrowser::FContentBrowserOperationContext&) -> bool;
		CONTENTBROWSERASSETDATASOURCE_API auto FixUpAllRedirectors(const ContentBrowser::FContentBrowserOperationContext&) -> bool;
		CONTENTBROWSERASSETDATASOURCE_API auto CanSaveItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::EContentBrowserItemSaveFlags, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto GetDataSourceName() const -> FName override;
		CONTENTBROWSERASSETDATASOURCE_API auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData&, bool, std::string_view, ContentBrowser::FContentBrowserItemDataAttributeValue&) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData&, bool, ContentBrowser::FContentBrowserItemDataAttributeValues&) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData&, std::string&) const -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CompileFilter(std::string_view, const ContentBrowser::FContentBrowserDataFilter&, ContentBrowser::FContentBrowserDataCompiledFilter&) const -> void override;
		CONTENTBROWSERASSETDATASOURCE_API auto ClearCachedFilterData() const -> void override;
		CONTENTBROWSERASSETDATASOURCE_API auto RenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto CopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto MoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> bool override;
		CONTENTBROWSERASSETDATASOURCE_API auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override;

	private:
		FDelegateHandle DirectoriesChanged, PackagesInvalidated, CatalogInvalidated, MountsInvalidated;
		std::shared_ptr<std::atomic_bool> FilterCacheInvalidated = std::make_shared<std::atomic_bool>(true);
		mutable std::shared_ptr<const ContentBrowser::AssetDataSource::Private::FContentBrowserAssetSnapshot> DiscoveryCatalog;
		FDelegateHandle QueryChanged;
	public:

	};
}
