#pragma once

#include "AssetCoreAPI.h"
#include "Asset/AuthoredBulkData.h"
#include "Asset/PackageInspection.h"

#include <filesystem>

namespace Durin::Asset
{
	inline constexpr std::string_view AuthoredBulkCompanionSuffix = ".dabulk";

	ASSETCORE_API auto ResolveAuthoredBulkCompanionPath(
		const std::filesystem::path& PackagePath,
		FXxHash128 ContainerHash,
		std::filesystem::path& OutPath,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto BuildAuthoredBulkCompanion(
		std::span<const FAuthoredBulkPayload> Payloads,
		FXxHash128 ContainerHash,
		std::vector<uint8>& OutBytes,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto ValidateAuthoredBulkCompanion(
		std::span<const uint8> Bytes,
		FXxHash128 ExpectedContainerHash,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto LoadAuthoredBulkPayload(
		const std::filesystem::path& CompanionPath,
		const FAuthoredBulkDataDescriptor& Descriptor,
		FSharedByteBuffer& OutBuffer,
		std::string* OutError = nullptr) -> bool;
	ASSETCORE_API auto InspectAuthoredBulkCompanionPaths(
		const std::filesystem::path& PackagePath,
		const FAssetPackageInspection& Inspection,
		std::vector<std::filesystem::path>& OutPaths,
		std::string* OutError = nullptr) -> bool;
}
