#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeItem.h"
#include "FileHelpers.h"
#include "Misc/FeedbackContext.h"
#include "ContentBrowserOperationContext.h"

namespace Durin::ContentBrowser::AssetDataSource::Private
{
	// Package save policy and execution only; no folder, file or deletion state.
	class FContentBrowserAssetSaveOperation
	{
	public:
		explicit FContentBrowserAssetSaveOperation(FContentBrowserOperationContext InHost = {}) : Host(std::move(InHost)) {}
		auto CanSaveItem(const FContentBrowserNativeItem& Item, bool IncludeClean = false, std::string* Error = nullptr) const -> bool;
		auto BulkSaveItems(std::span<const FContentBrowserNativeItem> Items, bool IncludeClean = false) -> bool;
	private:
		FContentBrowserOperationContext Host;
	};
}
