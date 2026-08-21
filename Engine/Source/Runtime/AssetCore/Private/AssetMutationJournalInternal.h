#pragma once

#include "AssetSystemInternal.h"
#include "Misc/Paths.h"

namespace Durin::Asset::Private
{
	enum class EAssetMutationState : uint8
	{
		Planned,
		Prepared,
		Publishing,
		Committed,
		Compensating,
		Restored,
		RecoveryRequired,
	};

	enum class ERelocationPublicationRole : uint8
	{
		RealAsset,
		OwnedPayload,
		Redirector,
	};

	struct FAssetMutationJournalEntry
	{
		std::filesystem::path PhysicalPath;
		FAssetPath RegistryPath;
		ERelocationPublicationRole Role = ERelocationPublicationRole::RealAsset;
		uint64 PublicationOrder = std::numeric_limits<uint64>::max();
		bool bPreExists = false;
		bool bPostExists = false;
		bool bCompleted = false;
		bool bCompensated = false;
		std::filesystem::path StagedPrePath;
		std::filesystem::path StagedPostPath;
		FXxHash128 StagedPreHash;
		FXxHash128 StagedPostHash;
		FAssetPackageFingerprint ExpectedPreFingerprint;
		FAssetPackageFingerprint ExpectedPostFingerprint;
	};

	// Retains every byte image required to compensate, undo, or redo one
	// authored mutation. Recovery-required roots deliberately outlive tokens.
	struct FAssetMutationJournal
	{
		std::string OperationId;
		std::string OperationType = "relocation";
		std::vector<std::filesystem::path> Roots;
		std::filesystem::path LocatorPath;
		std::vector<FAssetMutationJournalEntry> Entries;
		EAssetMutationState State = EAssetMutationState::Planned;

		~FAssetMutationJournal();
	};

	auto MakeRelocationOperationId() -> std::string;
	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path;
	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		std::vector<uint8>& OutBytes) -> FAssetResult;
	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		std::span<const uint8> Bytes) -> FAssetResult;
	auto FingerprintRelocationFile(
		const std::filesystem::path& Path,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult;
	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		std::span<const uint8> Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult;
	auto IsWritableRelocationPath(
		const std::filesystem::path& Path,
		const PathUtilities::FMountPoint*& OutMount,
		std::string& OutError) -> bool;
	auto WriteMutationJournalState(FAssetMutationJournal& Journal) -> void;
	auto PublishRelocationFile(
		const FAssetMutationJournalEntry& Entry,
		bool bForward) -> FAssetResult;
}
