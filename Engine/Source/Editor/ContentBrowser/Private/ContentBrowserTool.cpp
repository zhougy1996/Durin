#include "ContentBrowser/ContentBrowserTool.h"

#include "Asset/AssetReadResult.h"
#include "AssetTools/IAssetTools.h"
#include "Panels/ContentBrowserPanel.h"

namespace Durin::Editor::ContentBrowser
{
	auto CreateContentBrowserTool(
		FContentBrowserHostContext Services,
		FPresentationSettings Settings,
		FSavePresentationSettings SaveSettings)
		-> std::unique_ptr<IContentBrowserTool>
	{
		return std::make_unique<Private::FContentBrowserPanel>(
			std::move(Settings), std::move(SaveSettings), std::move(Services));
	}

} // namespace Durin::Editor::ContentBrowser
