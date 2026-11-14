#pragma once

#include "CoreMinimal.h"

#include "ContentBrowserDataSnapshot.h"

namespace Durin::ContentBrowser::Private
{
	using ContentBrowser::EEnumerationDiagnosticKind;
	using ContentBrowser::FEnumerationDiagnostic;
	using ContentBrowser::FEntryStatusQuery;
	// Selects the content category shown without changing directory navigation.
	enum class EContentBrowserTypeFilter : uint8
	{
		All,
		Assets,
		Files,
		Levels,
		StaticMeshes,
		Materials,
		Textures,
		OtherAssets,
		Redirectors
	};

	// Identifies the active content-browser sort key.
	enum class EContentBrowserSortColumn : uint8
	{
		Name,
		Type,
		Size,
		Modified
	};

}
