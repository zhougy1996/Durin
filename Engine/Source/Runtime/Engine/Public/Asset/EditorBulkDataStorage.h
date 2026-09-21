#pragma once

#include "EngineAPI.h"
#include "Asset/EditorBulkDataStorageError.h"
#include "DObject/PackageBulkStorage.h"
#include "Asset/PackageInspection.h"

namespace Durin
{
	ENGINE_API auto InspectEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths) -> FEditorBulkDataStorageResult;
	ENGINE_API auto InspectEditorBulkDataStorageDescriptors(
		const FAssetPackageInspection& Inspection,
		std::vector<FPackageBulkStorageDescriptor>& OutDescriptors) -> FEditorBulkDataStorageResult;
	ENGINE_API auto InspectOrphanedEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths) -> FEditorBulkDataStorageResult;
}
