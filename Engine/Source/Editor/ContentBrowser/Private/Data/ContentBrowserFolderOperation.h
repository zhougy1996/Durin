#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserMutationPolicy.h"
namespace Durin::ContentBrowser::Private
{
	class FContentBrowserFolderOperation : public FContentBrowserMutationPolicy
	{
	public:
		using FContentBrowserMutationPolicy::FContentBrowserMutationPolicy;
		auto ValidateFolder(std::string_view Directory, std::string_view Name, FContentBrowserNativeItem& OutNewItem) const -> FAssetWriteResult;
		auto PrepareFolder(std::string_view PhysicalDirectory, FContentBrowserNativeItem& OutNewItem) -> bool;
		auto CanCreateFolder(std::string_view PhysicalDirectory, std::string_view Name, std::string* OutErrorMsg = nullptr) const -> bool;
		auto CreateFolder(std::string_view PhysicalDirectory, std::string_view Name, FContentBrowserNativeItem& OutNewItem) -> bool;
	};
}
