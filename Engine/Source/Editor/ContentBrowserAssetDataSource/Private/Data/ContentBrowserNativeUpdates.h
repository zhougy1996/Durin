#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserDataSource.h"
#include "AssetRegistry/ContentChanges.h"
namespace Durin::ContentBrowser::AssetDataSource::Private
{
	auto QueueNativeCommittedUpdates(DContentBrowserDataSource&, const FContentChangeBatch&, bool FromDiscovery = false) -> void;
}
