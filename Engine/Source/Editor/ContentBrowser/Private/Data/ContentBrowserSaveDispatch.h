#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItemData.h"
#include "ContentBrowserOperationResult.h"

namespace Durin::ContentBrowser::Private
{
	auto DispatchSaveItems(std::span<const FContentBrowserItemData> Items, EContentBrowserItemSaveFlags Flags,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> FContentBrowserOperationResult;
}
