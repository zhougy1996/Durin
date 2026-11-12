#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserMutationPolicy.h"
#include "AssetTools/MutationTypes.h"
namespace Durin::ContentBrowser::Private
{
	class FContentBrowserAssetActions : public FContentBrowserMutationPolicy
	{
	public:
		using FContentBrowserMutationPolicy::FContentBrowserMutationPolicy;
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
