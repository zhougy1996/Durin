#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserNativeDataSource.gen.h"

namespace Durin::ContentBrowser::Private { struct FContentBrowserAssetSnapshot; }
namespace Durin
{
	// Reuses the existing mutation/recovery substrate through opaque native payloads.
	DCLASS(Abstract)
	class DContentBrowserNativeDataSource : public DContentBrowserDataSource
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserNativeDataSource(const FObjectInitializer&);
		CONTENTBROWSER_API auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserHostContext& = {}) -> void override;
		CONTENTBROWSER_API auto CreateItemData(const ContentBrowser::FContentBrowserItem&, const ContentBrowser::FContentBrowserHostContext& = {}) -> ContentBrowser::FContentBrowserItemData override;
		CONTENTBROWSER_API auto AreItemsEquivalentForOperations(const ContentBrowser::FContentBrowserItemData&, const ContentBrowser::FContentBrowserItemData&) const -> bool override;
		// Explicit transfer contract for the two native adapters sharing one atomic backend.
		CONTENTBROWSER_API auto CanHandleNativeBatch(std::span<const ContentBrowser::FContentBrowserItemData>) const -> bool;
		CONTENTBROWSER_API auto CanEditItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto EditItem(const ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSER_API auto CanDuplicateItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto DuplicateItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::FContentBrowserItem&) -> bool override;
		CONTENTBROWSER_API auto CanCreateFolder(std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto CreateFolder(std::string_view, ContentBrowser::FContentBrowserItemDataTemporaryContext&, const ContentBrowser::FContentBrowserHostContext& = {}) -> bool override;
		CONTENTBROWSER_API auto FixUpRedirectors(std::span<const FPackagePath>, const ContentBrowser::FContentBrowserHostContext&) -> bool;
		CONTENTBROWSER_API auto FixUpRedirectorsInFolder(std::string_view, const ContentBrowser::FContentBrowserHostContext&) -> bool;
		CONTENTBROWSER_API auto FixUpAllRedirectors(const ContentBrowser::FContentBrowserHostContext&) -> bool;
		CONTENTBROWSER_API auto CanSaveItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::EContentBrowserItemSaveFlags, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> bool override;
		CONTENTBROWSER_API auto CanRenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto RenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, ContentBrowser::FContentBrowserItem&) -> bool override;
		CONTENTBROWSER_API auto CanCopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override;
		CONTENTBROWSER_API auto CanMoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override;
		CONTENTBROWSER_API auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext override;
		CONTENTBROWSER_API auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> bool override;

	protected:
	};
	// Package-backed asset/redirector items and their logical folders.
	DCLASS()
	class DContentBrowserAssetDataSource : public DContentBrowserNativeDataSource
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserAssetDataSource(const FObjectInitializer&);
		CONTENTBROWSER_API auto GetDataSourceName() const -> FName override;
		CONTENTBROWSER_API auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData&, bool, std::string_view, ContentBrowser::FContentBrowserItemDataAttributeValue&) const -> bool override;
		CONTENTBROWSER_API auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData&, bool, ContentBrowser::FContentBrowserItemDataAttributeValues&) const -> bool override;
		CONTENTBROWSER_API auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData&, std::string&) const -> bool override;
		CONTENTBROWSER_API auto CompileFilter(std::string_view, const ContentBrowser::FContentBrowserDataFilter&, ContentBrowser::FContentBrowserDataCompiledFilter&) const -> void override;
		CONTENTBROWSER_API auto GetDiscoveryRevision() const -> uint64 override;
		CONTENTBROWSER_API auto IsDiscoveryPathAffected(uint64, std::string_view, bool) const -> bool override;
	private:
		mutable std::shared_ptr<const ContentBrowser::Private::FContentBrowserAssetSnapshot> DiscoveryCatalog;
	public:

	protected:
	};
	// Ordinary filesystem items and folders; package mutation stays in the asset substrate.
	DCLASS()
	class DContentBrowserFileDataSource : public DContentBrowserNativeDataSource
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserFileDataSource(const FObjectInitializer&);
		CONTENTBROWSER_API auto GetDataSourceName() const -> FName override;
		CONTENTBROWSER_API auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData&, bool, std::string_view, ContentBrowser::FContentBrowserItemDataAttributeValue&) const -> bool override;
		CONTENTBROWSER_API auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData&, bool, ContentBrowser::FContentBrowserItemDataAttributeValues&) const -> bool override;
		CONTENTBROWSER_API auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData&, std::string&) const -> bool override;
		CONTENTBROWSER_API auto CompileFilter(std::string_view, const ContentBrowser::FContentBrowserDataFilter&, ContentBrowser::FContentBrowserDataCompiledFilter&) const -> void override;

	protected:
	};
} // namespace Durin
