#pragma once

#include "Panels/ContentBrowserQuery.h"

namespace Durin::Editor::ContentBrowser::Private
{
	// Navigation and query choices outlive individual captures and projections.
	// Panel selection and widget presentation remain owned by the panel.
	struct FContentBrowserSession
	{
		std::string CurrentPhysicalPath;
		std::string CurrentVirtualPath;
		std::vector<std::string> NavigationHistory;
		int32 HistoryIndex = -1;
		uint64 NavigationRevision = 0;
		FContentBrowserQuerySettings Query;
	};
}
