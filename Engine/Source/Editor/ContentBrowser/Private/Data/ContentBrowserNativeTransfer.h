#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItemDataTransferContext.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserOperationContext.h"

namespace Durin::ContentBrowser::Private
{
	struct FNativeTransferInput
	{
		FContentBrowserNativeItem Item;
		FContentBrowserItemContributionKey Identity;
		std::shared_ptr<const FContentBrowserOperationContext> Policy;
	};
	auto PrepareFileTransfer(std::vector<FNativeTransferInput> Inputs, std::string Destination, bool Move, std::optional<std::string> NewName = {})
		-> std::unique_ptr<FContentBrowserItemDataTransferContext>;
	auto PrepareAssetTransfer(std::vector<FNativeTransferInput> Inputs, std::string Destination, bool Move, std::optional<std::string> NewName = {})
		-> std::unique_ptr<FContentBrowserItemDataTransferContext>;
}
