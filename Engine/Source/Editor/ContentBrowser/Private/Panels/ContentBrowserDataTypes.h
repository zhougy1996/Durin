#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetReadResult.h"
#include "ContentBrowserDataCapture.h"

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

	// Value-owned capture; publication never mutates a snapshot held by a reader.
	struct FContentBrowserItemsSnapshot
	{
		uint64 Version = 0, QueryRevision = 0;
		bool bStale = false;
		std::vector<FContentBrowserItem> Items;
		struct FContribution
		{
			TWeakObjectPtr<DContentBrowserDataSource> Source;
			uint64 Generation = 0, MountRevision = 0;
			size_t Begin = 0, End = 0;
			std::string Project;
		};
		std::vector<FContribution> Contributions;
		std::vector<FEnumerationDiagnostic> Diagnostics;
		size_t SuppressedDiagnosticCount = 0;
	};

	// Children remain valid as long as the caller retains the published owner.
	struct FContentBrowserDirectorySnapshot
	{
		uint64 Version = 0, QueryRevision = 0;
		bool bStale = false;
		std::vector<std::filesystem::path> Children;
		std::shared_ptr<FContentBrowserItemsSnapshot> Capture;
		std::vector<FEnumerationDiagnostic> Diagnostics;
		size_t SuppressedDiagnosticCount = 0;
	};

}
