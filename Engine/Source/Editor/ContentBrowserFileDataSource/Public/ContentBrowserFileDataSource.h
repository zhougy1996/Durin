#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserFileDataSourceAPI.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserFileDataSource.gen.h"
namespace Durin::ContentBrowser::FileDataSource::Private { struct FContentBrowserAssetSnapshot; struct FContentBrowserFileUpdateState; }
namespace Durin
{
	DCLASS()
	class DContentBrowserFileDataSource : public DContentBrowserDataSource
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSERFILEDATASOURCE_API explicit DContentBrowserFileDataSource(const FObjectInitializer&);
		CONTENTBROWSERFILEDATASOURCE_API auto Initialize(bool AutoRegister = true) -> void override;
		CONTENTBROWSERFILEDATASOURCE_API auto Shutdown() -> void override;
		CONTENTBROWSERFILEDATASOURCE_API auto Tick(float DeltaSeconds) -> void override;
		CONTENTBROWSERFILEDATASOURCE_API auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItemData&&)>&) -> void override;
		CONTENTBROWSERFILEDATASOURCE_API auto BindItemData(const ContentBrowser::FContentBrowserItemData&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> ContentBrowser::FContentBrowserItemData override;
		CONTENTBROWSERFILEDATASOURCE_API auto CanCreateFolder(std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto CreateFolder(std::string_view, ContentBrowser::FContentBrowserItemDataTemporaryContext&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext override;
		CONTENTBROWSERFILEDATASOURCE_API auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> bool override;

		CONTENTBROWSERFILEDATASOURCE_API auto CanRenameItem(const ContentBrowser::FContentBrowserItemData&, std::optional<std::string_view> = std::nullopt, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto CanCopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto CanMoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto CanEditItem(const ContentBrowser::FContentBrowserItemData&, std::string* = nullptr) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto EditItem(const ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto GetDataSourceName() const -> FName override;
		CONTENTBROWSERFILEDATASOURCE_API auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData&, bool, std::string_view, ContentBrowser::FContentBrowserItemDataAttributeValue&) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData&, bool, ContentBrowser::FContentBrowserItemDataAttributeValues&) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData&, std::string&) const -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto CompileFilter(std::string_view, const ContentBrowser::FContentBrowserDataFilter&, ContentBrowser::FContentBrowserDataCompiledFilter&) const -> void override;

		CONTENTBROWSERFILEDATASOURCE_API auto RenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, ContentBrowser::FContentBrowserItemData&) -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto CopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto MoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> bool override;
		CONTENTBROWSERFILEDATASOURCE_API auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool override;

	private:
		CONTENTBROWSERFILEDATASOURCE_API auto ResetDiscovery() -> void;
		FDelegateHandle DirectoriesChanged, MountsChanged;
		std::shared_ptr<ContentBrowser::FileDataSource::Private::FContentBrowserFileUpdateState> Updates;
		friend auto RecordContentBrowserFileUpdates(DContentBrowserFileDataSource&, const FContentChangeBatch&, bool) -> void;

	};
}
