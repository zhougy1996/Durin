#pragma once

#include "EngineAPI.h"
#include "Misc/FilePath.h"
#include "Asset/EditorBulkDataStorageError.h"
#include "DObject/PackageBulkStorage.h"
#include "Asset/PackageInspection.h"

namespace Durin
{
	[[nodiscard]] ENGINE_API auto InspectEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection) -> std::expected<std::vector<FFilePath>, FEditorBulkDataStorageError>;
	[[nodiscard]] ENGINE_API auto InspectEditorBulkDataStorageDescriptors(
		const FAssetPackageInspection& Inspection) -> std::expected<std::vector<FPackageBulkStorageDescriptor>, FEditorBulkDataStorageError>;
	[[nodiscard]] ENGINE_API auto InspectOrphanedEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection) -> std::expected<std::vector<FFilePath>, FEditorBulkDataStorageError>;
}
