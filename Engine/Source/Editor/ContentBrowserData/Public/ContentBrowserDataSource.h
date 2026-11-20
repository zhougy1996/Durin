#pragma once
#include "CoreMinimal.h"
#include "DObject/Object.h"
#include "DObject/StrongObjectPtr.h"
#include "Modules/ModularFeature.h"
#include "ContentBrowserItemData.h"
#include "ContentBrowserDataCapture.h"
#include "ContentBrowserOperationContext.h"
#include "ContentBrowserDataSource.gen.h"

namespace Durin::ContentBrowser
{
	// Receives invalidation from active providers, independently of browser panels.
	class IContentBrowserItemDataSink
	{
	public:
		virtual ~IContentBrowserItemDataSink() = default;
		virtual auto QueueItemDataUpdate(FContentBrowserItemDataUpdate&& Update) -> void = 0;
		virtual auto NotifyItemDataRefreshed() -> void = 0;
	};

	// Source-owned validation and deferred creation; constructing a context performs no mutation.
	class FContentBrowserItemDataTemporaryContext
	{
	public:
		FContentBrowserItemDataTemporaryContext() = default;
		FContentBrowserItemDataTemporaryContext(FContentBrowserItemData InItem, std::function<bool(std::string_view, std::string*)> InValidate, std::function<bool(std::string_view, FContentBrowserItemData&)> InFinalize)
			: Item(std::move(InItem))
			, Validate(std::move(InValidate))
			, Finalize(std::move(InFinalize))
		{
		}
		auto GetItemData() const -> const FContentBrowserItemData& { return Item; }
		auto ValidateItem(std::string_view Name, std::string* Error = nullptr) const -> bool { return Validate && Validate(Name, Error); }
		auto FinalizeItem(std::string_view Name, FContentBrowserItemData& Out) -> bool
		{
			if (!ValidateItem(Name) || !Finalize || !Finalize(Name, Out)) return false;
			Validate = {};
			Finalize = {};
			return true;
		}

	private:
		FContentBrowserItemData Item;
		std::function<bool(std::string_view, std::string*)> Validate;
		std::function<bool(std::string_view, FContentBrowserItemData&)> Finalize;
	};
} // namespace Durin::ContentBrowser
namespace Durin
{
	class DEditorEngine;
	// UE-shaped provider boundary. Providers register instances, not factories.
	DCLASS(Abstract)
	class DContentBrowserDataSource : public DObject, public IModularFeature
	{
		GENERATED_BODY()
	public:
		static constexpr std::string_view FeatureName = "ContentBrowserDataSource";
		CONTENTBROWSERDATA_API explicit DContentBrowserDataSource(const FObjectInitializer& Initializer);
		CONTENTBROWSERDATA_API virtual auto Initialize(bool bAutoRegister = true) -> void;
		CONTENTBROWSERDATA_API virtual auto Shutdown() -> void;
		CONTENTBROWSERDATA_API auto BeginDestroy() -> void override;
		CONTENTBROWSERDATA_API static auto OnDataSourceMembershipChanged() -> TMulticastDelegate<void()>&;
		CONTENTBROWSERDATA_API auto RegisterDataSource() -> void;
		CONTENTBROWSERDATA_API auto UnregisterDataSource() -> void;
		CONTENTBROWSERDATA_API auto SetDataSink(ContentBrowser::IContentBrowserItemDataSink* Sink) -> void;
		auto IsInitialized() const -> bool { return bInitialized; }
		CONTENTBROWSERDATA_API auto IsActive() const -> bool;
		CONTENTBROWSERDATA_API auto GetOwningEditor() const -> DEditorEngine*;
		CONTENTBROWSERDATA_API virtual auto GetDataSourceName() const -> FName;
		virtual auto Tick(float) -> void {}
		CONTENTBROWSERDATA_API virtual auto GetItemAttribute(const ContentBrowser::FContentBrowserItemData&, bool IncludeMetaData, std::string_view Key, ContentBrowser::FContentBrowserItemDataAttributeValue& Out) const -> bool;
		CONTENTBROWSERDATA_API virtual auto GetItemAttributes(const ContentBrowser::FContentBrowserItemData&, bool IncludeMetaData, ContentBrowser::FContentBrowserItemDataAttributeValues& Out) const -> bool;
		CONTENTBROWSERDATA_API virtual auto GetItemPhysicalPath(const ContentBrowser::FContentBrowserItemData&, std::string& Out) const -> bool;
		virtual auto ClearCachedFilterData() const -> void {}
		CONTENTBROWSERDATA_API auto QueueItemDataUpdate(ContentBrowser::FContentBrowserItemDataUpdate&& Update) -> void;
		CONTENTBROWSERDATA_API auto NotifyItemDataRefreshed() -> void;
		CONTENTBROWSERDATA_API virtual auto CompileFilter(std::string_view Path, const ContentBrowser::FContentBrowserDataFilter& Filter, ContentBrowser::FContentBrowserDataCompiledFilter& Out) const -> void;
		// Owner-thread checks for a complete delta; must agree with enumeration.
		CONTENTBROWSERDATA_API virtual auto DoesItemPassFilter(const ContentBrowser::FContentBrowserItemData& Item, const ContentBrowser::FContentBrowserDataCompiledFilter& Filter) const -> bool;
		virtual auto ConvertItemForFilter(ContentBrowser::FContentBrowserItemData&, const ContentBrowser::FContentBrowserDataCompiledFilter&) const -> bool { return false; }
		CONTENTBROWSERDATA_API virtual auto EnumerateItemsMatchingFilter(const ContentBrowser::FContentBrowserDataCompiledFilter& Filter, const std::function<bool(ContentBrowser::FContentBrowserItemData&&)>& Callback) -> void;
		// Bind an immutable description to this provider and the caller's policy.
		// Bound inputs must belong to this provider; foreign inputs reject.
		// Empty policy preserves an already bound item. Feedback updates never
		// remove existing restrictions; requested restrictions are conjunctive.
		CONTENTBROWSERDATA_API virtual auto BindItemData(const ContentBrowser::FContentBrowserItemData& Item, const ContentBrowser::FContentBrowserOperationContext& Host = {}) -> ContentBrowser::FContentBrowserItemData;
		CONTENTBROWSERDATA_API auto EnumerateItemsAtPath(std::string_view Path, const std::function<bool(ContentBrowser::FContentBrowserItemData&&)>& Callback) -> void;
		CONTENTBROWSERDATA_API virtual auto CanEditItem(const ContentBrowser::FContentBrowserItemData&, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto EditItem(const ContentBrowser::FContentBrowserItemData&) -> bool;
		CONTENTBROWSERDATA_API virtual auto CanDuplicateItem(const ContentBrowser::FContentBrowserItemData&, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto DuplicateItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::FContentBrowserItemData& Out) -> bool;
		CONTENTBROWSERDATA_API virtual auto CanCreateFolder(std::string_view Path, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto CreateFolder(std::string_view Path, ContentBrowser::FContentBrowserItemDataTemporaryContext& Out, const ContentBrowser::FContentBrowserOperationContext& Host = {}) -> bool;
		CONTENTBROWSERDATA_API virtual auto CanSaveItem(const ContentBrowser::FContentBrowserItemData&, ContentBrowser::EContentBrowserItemSaveFlags, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto BulkSaveItems(std::span<const ContentBrowser::FContentBrowserItemData>, ContentBrowser::EContentBrowserItemSaveFlags) -> bool;
		CONTENTBROWSERDATA_API virtual auto CanRenameItem(const ContentBrowser::FContentBrowserItemData&, std::optional<std::string_view> = std::nullopt, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto RenameItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, ContentBrowser::FContentBrowserItemData& Out) -> bool;
		CONTENTBROWSERDATA_API virtual auto CanCopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto CopyItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> bool;
		CONTENTBROWSERDATA_API virtual auto BulkCopyItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSERDATA_API virtual auto CanMoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto MoveItem(const ContentBrowser::FContentBrowserItemData&, std::string_view) -> bool;
		CONTENTBROWSERDATA_API virtual auto BulkMoveItems(std::span<const ContentBrowser::FContentBrowserItemData>, std::string_view) -> bool;
		CONTENTBROWSERDATA_API virtual auto CanDeleteItem(const ContentBrowser::FContentBrowserItemData&, std::string* Error = nullptr) const -> bool;
		CONTENTBROWSERDATA_API virtual auto DeleteItem(const ContentBrowser::FContentBrowserItemData&) -> bool;
		CONTENTBROWSERDATA_API virtual auto BulkDeleteItems(std::span<const ContentBrowser::FContentBrowserItemData>) -> bool;

	protected:
		ContentBrowser::IContentBrowserItemDataSink* DataSink = nullptr;

	private:
		bool bInitialized = false;
		FModularFeatureRegistration Registration;
		// Registration must survive ordinary GC before a subsystem discovers this instance.
		FStrongObjectPtr RegistrationRoot;
	};
} // namespace Durin
