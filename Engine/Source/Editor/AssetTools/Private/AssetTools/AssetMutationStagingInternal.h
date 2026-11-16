#pragma once

#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "Asset/LoadErrors.h"

#include "Asset/PackageInspection.h"
#include "AssetMutationResultInternal.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::AssetToolsPrivate
{
	enum class EAssetMutationPublicationRole : uint8
	{
		RealAsset,
		Redirector,
	};

	// Describes the complete before/after byte images for one physical mutation
	// participant without transferring their ownership.
	struct FMutationStageRequest
	{
		std::filesystem::path PhysicalPath;
		FPackagePath RegistryPath;
		EAssetMutationPublicationRole Role =
			EAssetMutationPublicationRole::RealAsset;
		bool bPreExists = false;
		bool bPostExists = false;
		FByteView PreBytes;
		FByteView PostBytes;
	};

	struct FAssetMutationStagingEntry
	{
		std::filesystem::path PhysicalPath;
		FPackagePath RegistryPath;
		EAssetMutationPublicationRole Role =
			EAssetMutationPublicationRole::RealAsset;
		bool bPreExists = false;
		bool bPostExists = false;
		std::filesystem::path StagedPostPath;
		FXxHash128 StagedPostHash;
		FAssetPackageFingerprint ExpectedPreFingerprint;
	};

	// Owns immutable publication inputs and the observed file ledger for one execution.
	// Partially published operations retain backup roots for manual repair only.
	struct FAssetMutationStaging
	{
		std::string OperationId;
		std::vector<std::filesystem::path> Roots;
		std::vector<FAssetMutationStagingEntry> Entries;
		// Normalized-path index for duplicate participant checks.
		std::unordered_map<std::string, size_t> EntryIndices;
		bool bRetainBackups = false;
		std::vector<std::filesystem::path> PublishedFiles;

		FAssetMutationStaging() = default;
		FAssetMutationStaging(const FAssetMutationStaging&) = delete;
		auto operator=(const FAssetMutationStaging&)
			-> FAssetMutationStaging& = delete;
		~FAssetMutationStaging();
	};

	auto InitializeMutationStaging(
		FAssetMutationStaging& Staging) -> void;
	// On success, publishes one complete entry, rejecting duplicate participants;
	// on failure, leaves no partial entry or unowned staging root.
	auto StageMutationEntry(
		FAssetMutationStaging& Staging,
		const FMutationStageRequest& Request,
		size_t& OutEntryIndex, std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;
	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path;
	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		FByteBuffer& OutBytes) -> std::expected<void, Durin::UnifiedError::FError>;
	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		FByteView Bytes, std::optional<UnifiedError::FError>* OutError = nullptr) -> bool;
	auto FingerprintRelocationFile(
		const std::filesystem::path& Path,
		FAssetPackageFingerprint& OutFingerprint) -> std::expected<void, Durin::UnifiedError::FError>;
	auto IsWritableRelocationPath(
		const std::filesystem::path& Path,
		const FMountPoint*& OutMount,
		std::string& OutError) -> bool;
	auto PublishRelocationFile(const FAssetMutationStagingEntry& Entry, std::optional<UnifiedError::FError>* OutError = nullptr)
		-> bool;
}
