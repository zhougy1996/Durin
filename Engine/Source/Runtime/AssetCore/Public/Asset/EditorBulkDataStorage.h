#pragma once

#include "AssetCoreAPI.h"
#include "Asset/EditorBulkDataStorageTypes.h"
#include "Asset/PackageInspection.h"

namespace Durin::Asset
{
	inline constexpr uint64 EditorBulkDataExternalThreshold = 256ull * 1024;
	inline constexpr std::string_view EditorBulkDataCompanionSuffix = ".dabulk";
	// Internal sibling retained only until the package/companion closure commits.
	inline constexpr std::string_view EditorBulkDataCompanionBackupSuffix = ".durin-backup";

	// Resolves the stable submitted companion independently of its container hash.
	ASSETCORE_API auto ResolveEditorBulkDataCompanionPath(
		const std::filesystem::path& PackagePath,
		std::filesystem::path& OutPath,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto BuildEditorBulkDataCompanion(
		std::span<const FEditorBulkDataStoragePayload> Payloads,
		FXxHash128 ContainerHash,
		std::vector<std::byte>& OutBytes,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto ValidateEditorBulkDataCompanion(
		std::span<const std::byte> Bytes,
		FXxHash128 ExpectedContainerHash,
		std::string* OutError = nullptr) -> bool;
	// Live load may restore a descriptor-matching backup and removes stale backups.
	ASSETCORE_API auto LoadEditorBulkDataStoragePayload(
		const std::filesystem::path& CompanionPath,
		const FEditorBulkDataStorageDescriptor& Descriptor,
		FSharedByteBuffer& OutBuffer,
		std::string* OutError = nullptr) -> bool;
	// Reads and verifies exactly one path without changing recovery state.
	ASSETCORE_API auto ReadEditorBulkDataStoragePayload(
		const std::filesystem::path& CompanionPath,
		const FEditorBulkDataStorageDescriptor& Descriptor,
		FSharedByteBuffer& OutBuffer,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto InspectEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto InspectEditorBulkDataStorageDescriptors(
		const FAssetPackageInspection& Inspection,
		std::vector<FEditorBulkDataStorageDescriptor>& OutDescriptors,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto InspectOrphanedEditorBulkDataCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError = nullptr) -> bool;
}
