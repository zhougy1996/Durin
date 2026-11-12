#pragma once
#include "CoreMinimal.h"
#include "Editor/EditorSubsystem.h"
#include "DObject/StrongObjectPtr.h"
#include "Delegates/Delegate.h"
#include "ContentBrowserDataSource.h"
#include "ContentBrowserDataSubsystem.gen.h"

namespace Durin::ContentBrowser::Private
{
	class FContentBrowserDataQuery;
}
namespace Durin
{
	// Per-editor source membership and aggregate query/operation routing.
	DCLASS()
	class DContentBrowserDataSubsystem : public DEditorSubsystem, public ContentBrowser::IContentBrowserItemDataSink
	{
		GENERATED_BODY()
	public:
		CONTENTBROWSER_API explicit DContentBrowserDataSubsystem(const FObjectInitializer&);
		CONTENTBROWSER_API auto Initialize(FSubsystemCollection&) -> void override;
		CONTENTBROWSER_API auto Deinitialize() noexcept -> void override;
		CONTENTBROWSER_API auto ActivateDataSource(FName Name) -> bool;
		CONTENTBROWSER_API auto DeactivateDataSource(FName Name) -> bool;
		CONTENTBROWSER_API auto GetActiveDataSource(FName Name) const -> DContentBrowserDataSource*;
		CONTENTBROWSER_API auto Tick(float DeltaSeconds) -> void;
		using FOnItemDataUpdated = TMulticastDelegate<void(std::span<const ContentBrowser::FContentBrowserItemDataUpdate>)>;
		auto OnItemDataUpdated() -> FOnItemDataUpdated& { return ItemDataUpdated; }
		CONTENTBROWSER_API auto QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate&& Update) -> void override;
		// All readers share this frame's immutable deltas until the next owner Tick.
		auto GetItemDataUpdates() const -> std::span<const ContentBrowser::FContentBrowserItemDataUpdate> { return PublishedUpdates; }
		CONTENTBROWSER_API auto GetRevision() -> uint64;
		CONTENTBROWSER_API auto IsActive() const -> bool;
		CONTENTBROWSER_API auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserHostContext& = {}) -> void;
		CONTENTBROWSER_API auto EnumerateItemsAtPath(std::string_view, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserHostContext& = {}) -> void;
		CONTENTBROWSER_API auto EnumerateItemsUnderPath(std::string_view Path, const ContentBrowser::FContentBrowserDataFilter&, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>&, const ContentBrowser::FContentBrowserHostContext& = {}) -> void;
		CONTENTBROWSER_API auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext;
		CONTENTBROWSER_API auto CanCreateFolder(std::string_view Path, std::string* Error = nullptr) -> bool;
		CONTENTBROWSER_API auto CreateFolder(std::string_view Path, ContentBrowser::FContentBrowserItemDataTemporaryContext& Out, const ContentBrowser::FContentBrowserHostContext& Host = {}) -> bool;
		CONTENTBROWSER_API auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> bool;
		CONTENTBROWSER_API auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSER_API auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSER_API auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> bool;

	private:
		friend class ContentBrowser::Private::FContentBrowserDataQuery;
		std::optional<uint64> LastBrowserFrame;
		auto RefreshSources() -> void;
		auto OwnsSource(const DContentBrowserDataSource*) const -> bool;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Builtins;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Available;
		std::vector<TStrongObjectPtr<DContentBrowserDataSource>> Active;
		uint64 Revision = 0;
		uint64 MountRevision = 0;
		uint64 SourceRevision = 0;
		FOnItemDataUpdated ItemDataUpdated;
		uint64 PendingFromRevision = 0, PublishedFromRevision = 0, PublishedToRevision = 0;
		std::vector<ContentBrowser::FContentBrowserItemDataUpdate> PendingUpdates;
		std::vector<ContentBrowser::FContentBrowserItemDataUpdate> PublishedUpdates;
		bool bStopping = false;
	};
} // namespace Durin
