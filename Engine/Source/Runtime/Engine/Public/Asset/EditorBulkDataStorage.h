#pragma once

#include "EngineAPI.h"
#include "Asset/EditorBulkDataStorageTypes.h"
#include "Asset/PackageInspection.h"

namespace Durin::Asset
{
	// Internal sibling retained only until the package/segment closure commits.
	inline constexpr std::string_view EditorBulkDataCompanionBackupSuffix =
		".durin-backup";

	ENGINE_API auto InspectEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError = nullptr) -> bool;
	ENGINE_API auto InspectEditorBulkDataStorageDescriptors(
		const FAssetPackageInspection& Inspection,
		std::vector<FEditorBulkDataStorageDescriptor>& OutDescriptors,
		std::string* OutError = nullptr) -> bool;
	ENGINE_API auto InspectOrphanedEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError = nullptr) -> bool;
}
