#include "ContentBrowser/ContentBrowserTool.h"

#include "Asset/AssetDefinitions.h"
#include "AssetTools/IAssetTools.h"
#include "Panels/ContentBrowserPanel.h"

namespace Durin::Editor::ContentBrowser
{
	auto CreateContentBrowserTool(
		FConstructionServices Services,
		FPresentationSettings Settings,
		FSavePresentationSettings SaveSettings)
		-> std::unique_ptr<IContentBrowserTool>
	{
		auto Panel = std::make_unique<::Durin::Editor::ContentBrowser::Private::FContentBrowserPanel>(
			std::move(Settings), std::move(SaveSettings),
			std::move(Services.OpenAsset), std::move(Services.CanMutateContent),
			std::move(Services.GetMountedContentMutationRevision),
			std::move(Services.NotifyMountedContentMutation),
			std::move(Services.QueryReimport),
			std::move(Services.Reimport),
			std::move(Services.ReconciliationState),
			std::move(Services.ThumbnailTaskScope));
		Panel->SetContentChangeServices(std::move(Services.CaptureMountedContentChanges),
			std::move(Services.NotifyScopedContentMutation));
		return Panel;
	}

} // namespace Durin::Editor::ContentBrowser
