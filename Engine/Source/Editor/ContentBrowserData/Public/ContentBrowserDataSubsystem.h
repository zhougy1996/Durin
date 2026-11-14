#pragma once
#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "TickableEditorObject.h"
#include "DObject/StrongObjectPtr.h"
#include "Delegates/Delegate.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserItem.h"
#include "ContentBrowserDataSubsystem.gen.h"

namespace Durin::ContentBrowser::Private
{
	class FContentBrowserDataQuery;
}
namespace Durin
{
	// Per-editor source membership and aggregate query/operation routing.
	DCLASS()
	class DContentBrowserDataSubsystem : public DEditorSubsystem, public ContentBrowser::IContentBrowserItemDataSink, public FTickableEditorObject
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSERDATA_API explicit DContentBrowserDataSubsystem(const FObjectInitializer&);
		CONTENTBROWSERDATA_API auto Initialize(FSubsystemCollection&) -> void override;
		CONTENTBROWSERDATA_API auto Deinitialize() noexcept -> void override;
		CONTENTBROWSERDATA_API auto ActivateDataSource(FName Name) -> bool;
		CONTENTBROWSERDATA_API auto DeactivateDataSource(FName Name) -> bool;
		CONTENTBROWSERDATA_API auto GetActiveDataSource(FName Name) const -> DContentBrowserDataSource*;
		CONTENTBROWSERDATA_API auto Tick(float DeltaSeconds) noexcept -> void override;
		auto GetTickableEditor() const -> DEditorEngine* override { return GetEditor(); }
		auto IsTickable() const -> bool override { return !bStopping && GetEditor(); }
		using FOnItemDataUpdated = TMulticastDelegate<void(std::span<const ContentBrowser::FContentBrowserItemDataUpdate>)>;
		auto OnItemDataUpdated() -> FOnItemDataUpdated& { return ItemDataUpdated; }
		auto OnItemDataRefreshed() -> TMulticastDelegate<void()>& { return ItemDataRefreshed; }
		CONTENTBROWSERDATA_API auto NotifyItemDataRefreshed() -> void override;
		CONTENTBROWSERDATA_API auto QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate&& Update) -> void override;
		auto HasPendingItemDataUpdates() const -> bool { return !PendingUpdates.empty(); }
		CONTENTBROWSERDATA_API auto IsActive() const -> bool;
		CONTENTBROWSERDATA_API auto CompileFilter(std::string_view, const ContentBrowser::FContentBrowserDataFilter&, ContentBrowser::FContentBrowserDataCompiledFilter&) -> void;
		CONTENTBROWSERDATA_API auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> void;
		CONTENTBROWSERDATA_API auto EnumerateItemsAtPath(std::string_view, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> void;
		CONTENTBROWSERDATA_API auto EnumerateItemsUnderPath(std::string_view Path, const ContentBrowser::FContentBrowserDataFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserOperationContext& = {}) -> void;
		CONTENTBROWSERDATA_API auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext;
		CONTENTBROWSERDATA_API auto CanCreateFolder(std::string_view Path, std::string* Error = nullptr) -> bool;
		CONTENTBROWSERDATA_API auto CreateFolder(std::string_view Path, ContentBrowser::FContentBrowserItemDataTemporaryContext& Out, const ContentBrowser::FContentBrowserOperationContext& Host = {}) -> bool;
		CONTENTBROWSERDATA_API auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> bool;
		CONTENTBROWSERDATA_API auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSERDATA_API auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSERDATA_API auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> bool;

	private:
		CONTENTBROWSERDATA_API static auto MergeItem(std::vector<ContentBrowser::FContentBrowserItem>&, ContentBrowser::FContentBrowserItem) -> void;
		friend class ContentBrowser::Private::FContentBrowserDataQuery;
		FDelegateHandle SourceMembershipChanged, MountsChanged;
		auto RefreshSources() -> void;
		auto OwnsSource(const DContentBrowserDataSource*) const -> bool;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Available;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Active;
		FOnItemDataUpdated ItemDataUpdated;
		TMulticastDelegate<void()> ItemDataRefreshed;
		bool bPendingItemDataRefreshed = false;
		std::vector<ContentBrowser::FContentBrowserItemDataUpdate> PendingUpdates;
		bool bStopping = true;
	};
} // namespace Durin
