#pragma once

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Mutation.h"
#undef DURIN_ENGINE_ASSET_INTERNAL
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin::AssetPrivate
{
	enum class EAssetMutationPublicationRole : uint8
	{
		RealAsset,
		OwnedPayload,
		Redirector,
	};

	// Selects whether an already-staged physical participant is a conflict or
	// may reuse an identical byte-image plan.
	enum class EMutationStagingDuplicatePolicy : uint8
	{
		Reject,
		ReuseEquivalent,
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
		EMutationStagingDuplicatePolicy DuplicatePolicy =
			EMutationStagingDuplicatePolicy::Reject;
	};

	struct FAssetMutationStagingEntry
	{
		std::filesystem::path PhysicalPath;
		FPackagePath RegistryPath;
		EAssetMutationPublicationRole Role =
			EAssetMutationPublicationRole::RealAsset;
		bool bPreExists = false;
		bool bPostExists = false;
		std::filesystem::path StagedPrePath;
		std::filesystem::path StagedPostPath;
		FXxHash128 StagedPreHash;
		FXxHash128 StagedPostHash;
		FAssetPackageFingerprint ExpectedPreFingerprint;
	};

	// Retains staged inputs and in-process progress for one authored mutation.
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
	// On success, publishes one complete entry or reuses an equivalent entry;
	// on failure, leaves no partial entry or unowned staging root.
	auto StageMutationEntry(
		FAssetMutationStaging& Staging,
		const FMutationStageRequest& Request,
		size_t& OutEntryIndex) -> FAssetWriteResult;
	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path;
	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		FByteBuffer& OutBytes) -> FAssetReadResult;
	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		FByteView Bytes) -> FAssetWriteResult;
	auto FingerprintRelocationFile(
		const std::filesystem::path& Path,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult;
	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		FByteView Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetReadResult;
	auto IsWritableRelocationPath(
		const std::filesystem::path& Path,
		const FMountPoint*& OutMount,
		std::string& OutError) -> bool;
	auto PublishRelocationFile(const FAssetMutationStagingEntry& Entry)
		-> FAssetWriteResult;
}
