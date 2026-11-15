#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSubsystem.h"
#include "ContentBrowserDataSnapshot.h"
namespace Durin::ContentBrowser::Private
{
	class CONTENTBROWSERDATA_API FContentBrowserDataQuery
	{
		friend struct FContentBrowserDataQueryFixtureAccess;
	  public:
		explicit FContentBrowserDataQuery(TWeakObjectPtr<DContentBrowserDataSubsystem>, FContentBrowserOperationContext = {});
		~FContentBrowserDataQuery();
		FContentBrowserDataQuery(const FContentBrowserDataQuery&) = delete;
		auto operator=(const FContentBrowserDataQuery&) -> FContentBrowserDataQuery& = delete;
		auto FindSourceKey(FName Name) const -> FObjectKey;
		auto IsAcceptingRequests() const -> bool;
		auto StopRequestAdmission() -> void { bAccepting = false; }
		auto GetDataSubsystem() const -> DContentBrowserDataSubsystem* { return Service.Get(); }
		auto IsProviderDirectory(std::string_view Virtual) const -> bool;
		auto ProjectItems(FContentBrowserItemsCapture, std::string_view, bool) const -> FContentBrowserItemsSnapshot;
		auto ProjectItem(ContentBrowser::FContentBrowserItemData Item, const ContentBrowser::FContentBrowserDataCompiledFilter& Filter) const -> std::optional<ContentBrowser::FContentBrowserItemData>;
		auto ProjectDirectory(FContentBrowserDirectorySnapshot, std::string_view) const -> FContentBrowserDirectorySnapshot;
		using FItemsCapture = TMoveOnlyFunction<std::unique_ptr<FContentBrowserItemsCapture>()>;
		using FDirectoryCapture = TMoveOnlyFunction<std::unique_ptr<FContentBrowserDirectorySnapshot>()>;
		auto PrepareItemsCapture(std::string Virtual, bool Recursive, FEntryStatusQuery Query = {},
								 std::shared_ptr<Tasks::FCancellationToken> Cancellation = {}) const -> FItemsCapture;
		auto PrepareDirectoryCapture(std::string Virtual, FEntryStatusQuery Query = {},
									 std::shared_ptr<Tasks::FCancellationToken> Cancellation = {}) const -> FDirectoryCapture;

	  private:
		auto GetSources() const -> std::vector<TStrongObjectPtr<DContentBrowserDataSource>>;
		auto OwnsSource(const DContentBrowserDataSource*) const -> bool;
		TWeakObjectPtr<DContentBrowserDataSubsystem> Service;
		FContentBrowserOperationContext Policy;
		struct FPendingCapture { std::weak_ptr<Tasks::FCancellationToken> Cancellation; };
		mutable std::vector<FPendingCapture> PendingCaptures;
		std::shared_ptr<void> Lifetime = std::make_shared<int>(0);
		FDelegateHandle Refreshed;
		bool bAccepting = true;
	};
} // namespace Durin::ContentBrowser::Private
