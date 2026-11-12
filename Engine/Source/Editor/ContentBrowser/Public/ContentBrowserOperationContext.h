#pragma once
#include "CoreMinimal.h"
#include "DObject/WeakObjectPtr.h"
#include "AssetRegistry/ContentChanges.h"
namespace Durin { class DAssetEditorSubsystem; }
namespace Durin::ContentBrowser
{
	// Operation policy and feedback only. Discovery requires no browser services.
	struct FContentBrowserOperationContext
	{
		std::function<bool(const std::string&, const std::string&)> OpenAsset;
		std::function<void()> NotifyMountedContentMutation;
		std::function<void(FContentChangeBatch)> NotifyScopedContentMutation;
		std::function<bool()> CanMutateContent;
		TWeakObjectPtr<DAssetEditorSubsystem> AssetEditors;
		std::function<void(ELogLevel, std::string_view)> ReportDiagnostic;
	};
}
