#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetReadResult.h"
#include "ContentBrowserItem.h"

namespace Durin::ContentBrowser::Private
{
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

	enum class EEnumerationDiagnosticKind : uint8
	{
		Entry,
		Traversal,
	};

	struct FEnumerationDiagnostic
	{
		EEnumerationDiagnosticKind Kind = EEnumerationDiagnosticKind::Entry;
		std::string PhysicalPath;
		std::string Message;
	};

	using FEntryStatusQuery = std::function<std::filesystem::file_status(
		const std::filesystem::directory_entry&,
		std::error_code&)>;

	// Value-owned capture; publication never mutates a snapshot held by a reader.
	struct FContentBrowserItemsSnapshot
	{
		uint64 Version = 0;
		std::vector<FContentBrowserItem> Items;
		std::vector<FEnumerationDiagnostic> Diagnostics;
		size_t SuppressedDiagnosticCount = 0;
	};

	// Children remain valid as long as the caller retains the published owner.
	struct FContentBrowserDirectorySnapshot
	{
		uint64 Version = 0;
		std::vector<std::filesystem::path> Children;
		std::vector<FEnumerationDiagnostic> Diagnostics;
		size_t SuppressedDiagnosticCount = 0;
	};

}
