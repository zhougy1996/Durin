#pragma once

#include "Asset/AssetDefinitions.h"
#include "DObject/AssetPath.h"
#include <filesystem>

namespace Durin::Editor::ContentBrowser::Private
{
	// Distinguishes folders, registered assets, and ordinary files.
	enum class EContentBrowserItemKind : uint8
	{
		Folder,
		Asset,
		Redirector,
		File
	};

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

	// Captures one mounted content item and its searchable metadata.
	struct FContentBrowserItem
	{
		EContentBrowserItemKind Kind = EContentBrowserItemKind::File;
		std::string Name;
		std::string VirtualPath;
		FPackagePath PackagePath;
		std::string PhysicalPath;
		std::string AssetClassName;
		std::string Extension;
		FObjectPath RedirectDestination;
		std::string ThumbnailIdentity;
		std::string ThumbnailSourcePath;
		uintmax_t ThumbnailFileSize = 0;
		std::filesystem::file_time_type ThumbnailLastWriteTime{};
		uint32 ThumbnailPackageFormatVersion = 0;
		int64 ThumbnailLastWriteTimeTicks = 0;
		uintmax_t FileSize = 0;
		std::filesystem::file_time_type LastWriteTime{};

		auto StableId() const -> const std::string&
		{
			return Kind == EContentBrowserItemKind::Asset
				|| Kind == EContentBrowserItemKind::Redirector
				? VirtualPath : PhysicalPath;
		}
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
