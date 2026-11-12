#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSource.h"
namespace Durin::ContentBrowser::Private
{
	auto QueueNativeCommittedUpdates(DContentBrowserDataSource&, const FContentChangeBatch&, bool FromDiscovery = false) -> void;
}
