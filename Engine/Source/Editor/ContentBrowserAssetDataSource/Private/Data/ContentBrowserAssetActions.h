#pragma once
#include "CoreMinimal.h"
#include "AssetTools/ContentMutation.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserOperationContext.h"
#include "AssetTools/MutationTypes.h"
namespace Durin::ContentBrowser::AssetDataSource::Private
{
	class FContentBrowserAssetActions : public ContentStorage::FContentMutationOperation
	{
	public:
		explicit FContentBrowserAssetActions(const FContentBrowserOperationContext& Host)
			: FContentMutationOperation({Host.CanMutateContent, Host.ReportDiagnostic, Host.NotifyContentMutation}) {}
		auto DuplicateItem(const FContentBrowserNativeItem& Item, FContentBrowserNativeItem& OutNewItem) -> bool;
		auto DuplicateItem(const FContentBrowserNativeItem& Item, std::string_view PhysicalDirectory, FContentBrowserNativeItem& OutNewItem) -> bool;
		auto CollectRedirectors(std::string_view VirtualDirectory) const
			-> std::vector<FPackagePath>;
		auto FixUpRedirectorsInFolder(std::string_view VirtualDirectory) -> bool;
		auto FixUpRedirectors(std::span<const FPackagePath> Redirectors) -> bool;
		auto FixUpAllRedirectors() -> bool;
		auto CanDuplicateAsset(const FTopLevelAssetPath& Source, std::string* OutErrorMsg) const -> bool;
		auto CanDuplicateItem(const FContentBrowserNativeItem& Item, std::string* OutErrorMsg = nullptr) const -> bool;
	private:
		auto ReportRecovery(const FAssetMutationResultDetails&) const -> void;
		auto CompleteMutation(const FAssetMutationResultDetails&, FContentChangeBatch Changes = {}) -> bool;
	};
}
