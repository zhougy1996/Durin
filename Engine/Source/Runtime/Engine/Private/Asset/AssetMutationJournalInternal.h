#pragma once

#define DURIN_ENGINE_ASSET_INTERNAL 1
#include "Asset/Mutation.h"
#undef DURIN_ENGINE_ASSET_INTERNAL
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

namespace Durin
{
	class FAssetPublicationCoordinator;
}

namespace Durin::AssetPrivate
{
	enum class EAssetMutationJournalKind : uint8
	{
		Relocation,
		RedirectorFixup,
	};

	enum class EAssetMutationState : uint8
	{
		Planned,
		Prepared,
		Publishing,
		Committed,
		RecoveryRequired,
	};

	enum class EAssetMutationPublicationRole : uint8
	{
		RealAsset,
		OwnedPayload,
		Redirector,
	};

	// Selects whether an already-staged physical participant is a conflict or
	// may reuse an identical byte-image plan.
	enum class EMutationJournalDuplicatePolicy : uint8
	{
		Reject,
		ReuseEquivalent,
	};

	// Describes the complete before/after byte images for one physical mutation
	// participant without transferring their ownership.
	struct FMutationJournalStageRequest
	{
		std::filesystem::path PhysicalPath;
		FPackagePath RegistryPath;
		EAssetMutationPublicationRole Role =
			EAssetMutationPublicationRole::RealAsset;
		bool bPreExists = false;
		bool bPostExists = false;
		std::span<const std::byte> PreBytes;
		std::span<const std::byte> PostBytes;
		EMutationJournalDuplicatePolicy DuplicatePolicy =
			EMutationJournalDuplicatePolicy::Reject;
	};

	struct FAssetMutationJournalEntry
	{
		std::filesystem::path PhysicalPath;
		FPackagePath RegistryPath;
		EAssetMutationPublicationRole Role =
			EAssetMutationPublicationRole::RealAsset;
		uint64 PublicationOrder = std::numeric_limits<uint64>::max();
		bool bPreExists = false;
		bool bPostExists = false;
		bool bCompleted = false;
		std::filesystem::path StagedPrePath;
		std::filesystem::path StagedPostPath;
		FXxHash128 StagedPreHash;
		FXxHash128 StagedPostHash;
		FAssetPackageFingerprint ExpectedPreFingerprint;
		FAssetPackageFingerprint ExpectedPostFingerprint;
	};

	struct FAssetMutationExternalParticipant
	{
		std::string ProviderId;
		std::string ExpectedFingerprint;
		std::vector<FAssetReferenceRewrite> Rewrites;
		bool bCompleted = false;
	};

	// Retains materialized inputs and durable forward progress for one authored
	// mutation. Recovery-required roots deliberately outlive tokens.
	struct FAssetMutationJournal
	{
		std::string OperationId;
		std::string OperationType;
		std::vector<std::filesystem::path> Roots;
		std::filesystem::path LocatorPath;
		std::vector<FAssetMutationJournalEntry> Entries;
		std::vector<FAssetMutationExternalParticipant> ExternalParticipants;
		// Transient normalized-path index; recovery records remain Entries-based.
		std::unordered_map<std::string, size_t> EntryIndices;
		EAssetMutationState State = EAssetMutationState::Planned;

		FAssetMutationJournal() = default;
		FAssetMutationJournal(const FAssetMutationJournal&) = delete;
		auto operator=(const FAssetMutationJournal&)
			-> FAssetMutationJournal& = delete;
		~FAssetMutationJournal();
	};

	auto InitializeMutationJournal(
		FAssetMutationJournal& Journal,
		EAssetMutationJournalKind OperationKind) -> void;
	// On success, publishes one complete entry or reuses an equivalent entry;
	// on failure, leaves no partial entry or unowned staging root.
	auto StageMutationJournalEntry(
		FAssetMutationJournal& Journal,
		const FMutationJournalStageRequest& Request,
		size_t& OutEntryIndex) -> FAssetResult;
	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path;
	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		FByteArray& OutBytes) -> FAssetResult;
	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		std::span<const std::byte> Bytes) -> FAssetResult;
	auto FingerprintRelocationFile(
		const std::filesystem::path& Path,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult;
	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		std::span<const std::byte> Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult;
	auto IsWritableRelocationPath(
		const std::filesystem::path& Path,
		const FMountPoint*& OutMount,
		std::string& OutError) -> bool;
	auto WriteMutationJournalState(FAssetMutationJournal& Journal) -> FAssetResult;
	// Makes a state visible only after every recovery record accepts it.
	auto TransitionMutationJournalState(
		FAssetMutationJournal& Journal,
		EAssetMutationState State) -> FAssetResult;
	auto IsMutationJournalRecoveryRequired(
		const FAssetMutationJournal& Journal) -> bool;
	auto RecoverPendingMutationJournals(
		FAssetPublicationCoordinator& Registry
	) -> FAssetResult;
	auto PublishRelocationFile(const FAssetMutationJournalEntry& Entry)
		-> FAssetResult;
}
