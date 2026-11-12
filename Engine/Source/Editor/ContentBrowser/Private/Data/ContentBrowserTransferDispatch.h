#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSource.h"

namespace Durin::ContentBrowser::Private
{
	auto DispatchTransferItems(std::span<const FContentBrowserItemData> Items, std::string_view Destination, bool Move,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> FContentBrowserOperationResult;
}
