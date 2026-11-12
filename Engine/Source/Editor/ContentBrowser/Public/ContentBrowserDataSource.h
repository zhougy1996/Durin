#pragma once
#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "DObject/StrongObjectPtr.h"
#include "Modules/ModularFeature.h"
#include "ContentBrowserItem.h"
#include "ContentBrowserDataCapture.h"
#include "ContentBrowser/ContentBrowserContracts.h"
#include "ContentBrowserDataSource.gen.h"

namespace Durin::ContentBrowser
{
	// Receives invalidation from active providers, independently of browser panels.
	class IContentBrowserItemDataSink
	{
	public:
		virtual ~IContentBrowserItemDataSink() = default;
		virtual auto QueueItemDataUpdate(FContentBrowserItemDataUpdate&& Update) -> void = 0;
	};
	struct FContentBrowserDeletePreview
	{
		struct FSummary
		{
			uint64 AssetCount = 0, FileCount = 0, CompanionCount = 0, FolderCount = 0;
		} Summary;
		struct FMessage
		{
			std::string DisplayName, PhysicalPath, RelatedAssetPath, Details;
		};
		std::string DisplayName;
		std::vector<FMessage> Blockers, Warnings;
	};
	// Source-owned validation and deferred creation; constructing a context performs no mutation.
	class FContentBrowserItemDataTemporaryContext
	{
	public:
		FContentBrowserItemDataTemporaryContext() = default;
		FContentBrowserItemDataTemporaryContext(FContentBrowserItem InItem, std::function<bool(std::string_view, std::string*)> InValidate, std::function<bool(std::string_view, FContentBrowserItem&)> InFinalize)
			: Item(std::move(InItem))
			, Validate(std::move(InValidate))
			, Finalize(std::move(InFinalize))
		{
		}
		auto GetItem() const -> const FContentBrowserItem& { return Item; }
		auto ValidateItem(std::string_view Name, std::string* Error = nullptr) const -> bool { return Validate && Validate(Name, Error); }
		auto FinalizeItem(std::string_view Name, FContentBrowserItem& Out) -> bool
		{
			if (!ValidateItem(Name) || !Finalize || !Finalize(Name, Out)) return false;
			Validate = {};
			Finalize = {};
			return true;
		}

	private:
		FContentBrowserItem Item;
		std::function<bool(std::string_view, std::string*)> Validate;
		std::function<bool(std::string_view, FContentBrowserItem&)> Finalize;
	};
	// Confirmed request capability; providers supply an owner-bound implementation.
	class IContentBrowserDeleteRequest
	{
	public:
		virtual ~IContentBrowserDeleteRequest() = default;
		virtual auto CanExecute() const -> bool = 0;
		virtual auto GetPreview() const -> std::shared_ptr<const FContentBrowserDeletePreview> { return {}; }
	};
	struct FContentBrowserDeleteContext
	{
		FContentBrowserItemData OwnerItem;
		std::unique_ptr<IContentBrowserDeleteRequest> Request;
		auto CanExecute() const -> bool { return OwnerItem.GetOwnerDataSource() && Request && Request->CanExecute(); }
	};
} // namespace Durin::ContentBrowser
namespace Durin
{
	// UE-shaped provider boundary. Providers register instances, not factories.
	DCLASS(Abstract)
	class DContentBrowserDataSource : public DObject, public IModularFeature
	{
		GENERATED_BODY()
	public:
		static constexpr std::string_view FeatureName = "ContentBrowserDataSource";
		static constexpr uint32 FeatureVersion = 3;
		CONTENTBROWSER_API explicit DContentBrowserDataSource(const FObjectInitializer& Initializer);
		CONTENTBROWSER_API auto Initialize(bool bAutoRegister = true) -> void;
		CONTENTBROWSER_API virtual auto Shutdown() -> void;
		CONTENTBROWSER_API auto BeginDestroy() -> void override;
		CONTENTBROWSER_API auto RegisterDataSource() -> void;
		CONTENTBROWSER_API auto UnregisterDataSource() -> void;
		CONTENTBROWSER_API auto SetDataSink(ContentBrowser::IContentBrowserItemDataSink* Sink) -> void;
		auto IsInitialized() const -> bool { return bInitialized; }
		auto GetActivationGeneration() const -> uint64 { return Generation; }
		CONTENTBROWSER_API auto IsActive() const -> bool;
		CONTENTBROWSER_API virtual auto GetDataSourceName() const -> FName;
		virtual auto Tick(float) -> void {}
		virtual auto GetDiscoveryRevision() const -> uint64 { return 0; }
		virtual auto IsDiscoveryPathAffected(uint64, std::string_view, bool) const -> bool { return true; }
		CONTENTBROWSER_API auto QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate&& Update) -> void;
		CONTENTBROWSER_API virtual auto CompileFilter(std::string_view Path, const ContentBrowser::FContentBrowserDataFilter& Filter, ContentBrowser::FContentBrowserDataCompiledFilter& Out) const -> void;
		CONTENTBROWSER_API virtual auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter& Filter, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>& Callback, const ContentBrowser::FContentBrowserHostContext& Host = {}) -> void;
		CONTENTBROWSER_API virtual auto CreateItemData(const ContentBrowser::FContentBrowserItem& Item, const ContentBrowser::FContentBrowserHostContext& Host = {}) -> ContentBrowser::FContentBrowserItemData;
		CONTENTBROWSER_API auto EnumerateItemsAtPath(std::string_view Path, const std::function<bool(ContentBrowser::FContentBrowserItem&&)>& Callback, const ContentBrowser::FContentBrowserHostContext& Host = {}) -> void;
		CONTENTBROWSER_API virtual auto CanEditItem(const ContentBrowser::FContentBrowserItemData&, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto EditItem(const ContentBrowser::FContentBrowserItemData&) -> bool;
		CONTENTBROWSER_API virtual auto CanDuplicateItem(const ContentBrowser::FContentBrowserItemData&, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto DuplicateItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::FContentBrowserItem& Out) -> bool;
		CONTENTBROWSER_API virtual auto CanCreateFolder(std::string_view Path, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto CreateFolder(std::string_view Path, ContentBrowser::FContentBrowserItemDataTemporaryContext& Out, const ContentBrowser::FContentBrowserHostContext& Host = {}) -> bool;
		CONTENTBROWSER_API virtual auto CanSaveItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::EContentBrowserItemSaveFlags, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> bool;
		CONTENTBROWSER_API virtual auto CanRenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto RenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, ContentBrowser::FContentBrowserItem& Out) -> bool;
		CONTENTBROWSER_API virtual auto CanCopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSER_API virtual auto CanMoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSER_API virtual auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData&, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSER_API virtual auto PrepareDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> ContentBrowser::FContentBrowserDeleteContext;
		CONTENTBROWSER_API virtual auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::FContentBrowserDeleteContext&) -> bool;

	protected:
		ContentBrowser::IContentBrowserItemDataSink* DataSink = nullptr;

	private:
		bool bInitialized = false;
		uint64 Generation = 0;
		FModularFeatureRegistration Registration;
		// Registration must survive ordinary GC before a subsystem discovers this instance.
		FStrongObjectPtr RegistrationRoot;
	};
} // namespace Durin
