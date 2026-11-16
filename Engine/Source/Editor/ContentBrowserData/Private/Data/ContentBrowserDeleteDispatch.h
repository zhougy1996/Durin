#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItemData.h"
namespace Durin::ContentBrowser::Private
{
	auto DispatchDeleteItems(std::span<const FContentBrowserItemData>,
		const std::function<bool(DContentBrowserDataSource*)>& Admitted) -> bool;
}
