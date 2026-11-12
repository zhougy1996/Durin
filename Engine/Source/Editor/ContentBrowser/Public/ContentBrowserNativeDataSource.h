#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserNativeDataSource.gen.h"

namespace Durin::ContentBrowser::Private { struct FContentBrowserAssetSnapshot; struct FContentBrowserFileUpdateState; }
namespace Durin
{
	// Package-backed asset/redirector items and their logical folders.
	DCLASS()
	class DContentBrowserAssetDataSource : public DContentBrowserDataSource
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserAssetDataSource(const FObjectInitializer&);
		CONTENTBROWSER_API auto Initialize(bool AutoRegister = true) -> void override;
		CONTENTBROWSER_API auto Shutdown() -> void override;
		CONTENTBROWSER_API auto Tick(float DeltaSeconds) -> void override;
		CONTENTBROWSER_API auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> void override;
		CONTENTBROWSER_API auto CreateItemData(const ContentBrowser::FContentBrowserItem&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> ContentBrowser::FContentBrowserItemData override;
		CONTENTBROWSER_API auto CanCreateFolder(std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto CreateFolder(std::string_view, ContentBrowser::FContentBrowserItemDataTemporaryContext&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> bool override;
		CONTENTBROWSER_API auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext override;
		CONTENTBROWSER_API auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> ContentBrowser::FContentBrowserOperationResult override;

		CONTENTBROWSER_API auto CanRenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto PrepareRenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override;
		CONTENTBROWSER_API auto CanCopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto CanMoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto PrepareCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override;
		CONTENTBROWSER_API auto PrepareMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override;
		CONTENTBROWSER_API auto CanEditItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto EditItem(const ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSER_API auto CanDuplicateItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto DuplicateItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSER_API auto FixUpRedirectors(std::span<const FPackagePath>, const ContentBrowser::FContentBrowserOperationContext&) -> bool;
		CONTENTBROWSER_API auto FixUpRedirectorsInFolder(std::string_view, const ContentBrowser::FContentBrowserOperationContext&) -> bool;
		CONTENTBROWSER_API auto FixUpAllRedirectors(const ContentBrowser::FContentBrowserOperationContext&) -> bool;
		CONTENTBROWSER_API auto CanSaveItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::EContentBrowserItemSaveFlags, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> ContentBrowser::FContentBrowserOperationResult override;
		CONTENTBROWSER_API auto GetDataSourceName() const -> FName override;
		CONTENTBROWSER_API auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData&, bool, std::string_view, ContentBrowser::FContentBrowserItemDataAttributeValue&) const -> bool override;
		CONTENTBROWSER_API auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData&, bool, ContentBrowser::FContentBrowserItemDataAttributeValues&) const -> bool override;
		CONTENTBROWSER_API auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData&, std::string&) const -> bool override;
		CONTENTBROWSER_API auto CompileFilter(std::string_view, const ContentBrowser::FContentBrowserDataFilter&, ContentBrowser::FContentBrowserDataCompiledFilter&) const -> void override;
		CONTENTBROWSER_API auto GetDiscoveryRevision() const -> uint64 override;
		CONTENTBROWSER_API auto IsDiscoveryPathAffected(uint64, std::string_view, bool) const -> bool override;
	private:
		mutable std::shared_ptr<const ContentBrowser::Private::FContentBrowserAssetSnapshot> DiscoveryCatalog;
		FDelegateHandle QueryChanged;
	public:

	protected:
	};
	// Ordinary filesystem items and folders; package mutation stays in the asset substrate.
	DCLASS()
	class DContentBrowserFileDataSource : public DContentBrowserDataSource
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserFileDataSource(const FObjectInitializer&);
		CONTENTBROWSER_API auto Initialize(bool AutoRegister = true) -> void override;
		CONTENTBROWSER_API auto Shutdown() -> void override;
		CONTENTBROWSER_API auto Tick(float DeltaSeconds) -> void override;
		CONTENTBROWSER_API auto GetDiscoveryRevision() const -> uint64 override;
		CONTENTBROWSER_API auto IsDiscoveryPathAffected(uint64, std::string_view, bool) const -> bool override;
		CONTENTBROWSER_API auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> void override;
		CONTENTBROWSER_API auto CreateItemData(const ContentBrowser::FContentBrowserItem&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> ContentBrowser::FContentBrowserItemData override;
		CONTENTBROWSER_API auto CanCreateFolder(std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto CreateFolder(std::string_view, ContentBrowser::FContentBrowserItemDataTemporaryContext&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> bool override;
		CONTENTBROWSER_API auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext override;
		CONTENTBROWSER_API auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> ContentBrowser::FContentBrowserOperationResult override;

		CONTENTBROWSER_API auto CanRenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto PrepareRenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override;
		CONTENTBROWSER_API auto CanCopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto CanMoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto PrepareCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override;
		CONTENTBROWSER_API auto PrepareMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> std::unique_ptr<ContentBrowser::FContentBrowserItemDataTransferContext> override;
		CONTENTBROWSER_API auto CanEditItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSER_API auto EditItem(const ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSER_API auto GetDataSourceName() const -> FName override;
		CONTENTBROWSER_API auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData&, bool, std::string_view, ContentBrowser::FContentBrowserItemDataAttributeValue&) const -> bool override;
		CONTENTBROWSER_API auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData&, bool, ContentBrowser::FContentBrowserItemDataAttributeValues&) const -> bool override;
		CONTENTBROWSER_API auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData&, std::string&) const -> bool override;
		CONTENTBROWSER_API auto CompileFilter(std::string_view, const ContentBrowser::FContentBrowserDataFilter&, ContentBrowser::FContentBrowserDataCompiledFilter&) const -> void override;

	private:
		std::shared_ptr<ContentBrowser::Private::FContentBrowserFileUpdateState> Updates;
		friend auto RecordContentBrowserFileUpdates(DContentBrowserFileDataSource&, const FContentChangeBatch&, bool) -> void;

	protected:
	};
} // namespace Durin
