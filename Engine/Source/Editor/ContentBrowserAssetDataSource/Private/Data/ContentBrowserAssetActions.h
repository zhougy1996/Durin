#pragma once
#include "CoreMinimal.h"
#include "AssetTools/ContentMutation.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserOperationContext.h"
namespace Durin::ContentBrowser::AssetDataSource::Private
{
	class FContentBrowserAssetActions
	{
	public:
		explicit FContentBrowserAssetActions(const FContentBrowserOperationContext& Host)
			: Policy{Host.CanMutateContent, Host.ReportDiagnostic, Host.NotifyContentMutation} {}
		auto DuplicateItem(const FContentBrowserNativeItem& Item, FContentBrowserNativeItem& OutNewItem) -> bool;
		auto DuplicateItem(const FContentBrowserNativeItem& Item, std::string_view PhysicalDirectory, FContentBrowserNativeItem& OutNewItem) -> bool;
		auto CollectRedirectors(std::string_view VirtualDirectory) const
			-> std::vector<FTopLevelAssetPath>;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory) -> bool;
		auto FixUpRedirectors(std::span<const FTopLevelAssetPath> Redirectors) -> bool;
		auto FixUpAllRedirectors() -> bool;
		auto CanDuplicateAsset(const FTopLevelAssetPath& Source, std::string* OutErrorMsg) const -> bool;
		auto CanDuplicateItem(const FContentBrowserNativeItem& Item, std::string* OutErrorMsg = nullptr) const -> bool;
	private:
		ContentStorage::FContentMutationPolicy Policy;
	};
}
