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
		return std::make_unique<::Durin::Editor::ContentBrowser::Private::FContentBrowserPanel>(
			std::move(Settings), std::move(SaveSettings),
			std::move(Services.OpenAsset), std::move(Services.CanMutateContent),
			std::move(Services.GetMountedContentMutationRevision),
			std::move(Services.NotifyMountedContentMutation),
			std::move(Services.QueryReimport),
			std::move(Services.Reimport),
			std::make_shared<::Durin::Editor::ContentBrowser::Private::FMountedContentReconciliationState>(),
			std::move(Services.ThumbnailTaskScope));
	}

} // namespace Durin::Editor::ContentBrowser
