#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserOperationContext.h"
namespace Durin::ContentBrowser::AssetDataSource::Private
{
	struct FNativeTransferInput { FContentBrowserNativeItem Item; std::shared_ptr<const FContentBrowserOperationContext> Policy; };
	auto ExecuteAssetTransfer(std::vector<FNativeTransferInput>, std::string, bool, std::optional<std::string> = {}) -> bool;
}
