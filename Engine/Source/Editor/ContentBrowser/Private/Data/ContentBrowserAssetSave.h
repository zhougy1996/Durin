#pragma once
#include "CoreMinimal.h"
#include "Data/ContentBrowserNativeItem.h"
#include "ContentBrowserOperationResult.h"
#include "AssetTools/AssetSave.h"
#include "Misc/FeedbackContext.h"
#include "ContentBrowserOperationContext.h"

namespace Durin::ContentBrowser::Private
{
	// Package save policy and execution only; no folder, file or deletion state.
	class FContentBrowserAssetSaveOperation
	{
	public:
		explicit FContentBrowserAssetSaveOperation(FContentBrowserOperationContext InHost = {}) : Host(std::move(InHost)) {}
		auto SetDiagnosticSink(FFeedbackContext::FMessageSink Sink) -> void { Host.ReportDiagnostic = std::move(Sink); }
		auto CanSaveItem(const FContentBrowserNativeItem& Item, EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage, std::string* Error = nullptr) const -> bool;
		auto BulkSaveItems(std::span<const FContentBrowserNativeItem> Items, EAssetSaveMode Mode = EAssetSaveMode::LoadedDirtyPackage) -> TContentBrowserOperationResult<FPackagePath>;
	private:
		FContentBrowserOperationContext Host;
	};
}
