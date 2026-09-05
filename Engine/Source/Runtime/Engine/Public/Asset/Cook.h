#pragma once

#include "Asset/AssetDefinitions.h"

#include "EngineAPI.h"
#include "Asset/CookedAsset.h"
#include "Asset/PackageBulkData.h"
#include "DObject/DObjectFwd.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"

namespace Durin
{
	struct FAssetPackageSerializationOptions;
	enum class ECookManifestEntryKind : uint8
	{
		CookedPackage = 1,
		CookedBulk = 2,
		PackageBulk = 3,
		ShaderLibrary = 4,
	};

	inline constexpr uint8 CookManifestEntryPresent = 1u << 0;
	inline constexpr uint8 CookManifestEntryCookedFieldProjection = 1u << 1;
	inline constexpr uint8 CookManifestEntryKnownFlags =
		CookManifestEntryPresent | CookManifestEntryCookedFieldProjection;

	struct FCookManifestEntry
	{
		ECookManifestEntryKind Kind = ECookManifestEntryKind::CookedPackage;
		uint8 Flags = CookManifestEntryPresent;
		std::string RelativePath;
		uint64 FileSize = 0;
		uint64 HashLow = 0;
		uint64 HashHigh = 0;

		auto operator==(const FCookManifestEntry&) const -> bool = default;
	};

	struct FCookManifest
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::vector<FCookManifestEntry> Entries;
	};

	ENGINE_API auto EncodeCookManifest(
		const FCookManifest& Manifest,
		FByteBuffer& OutBytes,
		std::string* OutError = nullptr
	) -> bool;
	ENGINE_API auto DecodeCookManifest(
		FByteView Bytes,
		FCookManifest& OutManifest,
		std::string* OutError = nullptr
	) -> bool;

	// Stable machine-readable terminal and per-package outcomes for project Cook.
	enum class ECookPackageStatus : uint8
	{
		CookHit,
		DdcHit,
		Rebuilt,
		ReusedOutput,
		Captured,
		Failed,
		Cancelled,
		Unsupported,
	};

	enum class ECookRunStatus : uint8
	{
		Succeeded,
		Failed,
		Cancelled,
	};

	enum class ECookIncrementalPolicy : uint8
	{
		Enabled,
		Disabled,
	};

	enum class ECookOperationStage : uint8
	{
		Discovery,
		Load,
		Prepare,
		Capture,
		StageSegment,
		StagePackage,
		StageAuxiliary,
		CommitSegment,
		CommitPackage,
		CommitAuxiliary,
		CommitState,
		CommitManifest,
		Rollback,
		StaleCleanup,
		WriterLock,
	};

	ENGINE_API auto CookPackageStatusName(ECookPackageStatus Status)
		-> std::string_view;
	ENGINE_API auto CookRunStatusName(ECookRunStatus Status) -> std::string_view;
	ENGINE_API auto CookOperationStageName(ECookOperationStage Stage)
		-> std::string_view;

	// Detached canonical bytes and integrity facts for one virtual package.
	struct FCookSavePlan
	{
		std::string VirtualPath;
		FPackagePath SourcePackagePath;
		FByteBuffer PackageBytes;
		FByteBuffer BulkBytes;
		FPackageBulkSegmentSummary BulkSummary;
		FXxHash128 InputFingerprint;
		FXxHash128 PackageDigest;
		FXxHash128 SegmentDigest;
		uint64 PackageFileSize = 0;
		uint64 SegmentFileSize = 0;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		uint32 FingerprintVersion = 1;
		uint32 ContributorVersion = 1;
		uint32 FamilyProducerVersion = 1;
		std::string Contributor;
		std::string BuildProvenance;
		bool bRawBulkSegment = false;
		bool bOpaqueRawSegment = false;
		bool bReuseExistingOutput = false;

		auto operator==(const FCookSavePlan&) const -> bool = default;
	};

	struct FCookPackageResult
	{
		FPackagePath RequestedRoot;
		FPackagePath PackagePath;
		std::string Contributor;
		std::string Code;
		std::string Diagnostic;
		ECookPackageStatus Status = ECookPackageStatus::Failed;
		ECookOperationStage Stage = ECookOperationStage::Discovery;
		uint64 PackageBytes = 0;
		uint64 SegmentBytes = 0;
	};

	struct FCookProgress
	{
		ECookOperationStage Stage = ECookOperationStage::Discovery;
		FPackagePath PackagePath;
		uint64 CompletedPackages = 0;
		uint64 TotalPackages = 0;
	};

	using FCookCancellationCheck = std::function<bool()>;
	using FCookProgressCallback = std::function<void(const FCookProgress&)>;

	struct FCookRequest
	{
		std::filesystem::path OutputRoot;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::vector<FPackagePath> ExplicitRoots;
		ECookIncrementalPolicy IncrementalPolicy = ECookIncrementalPolicy::Enabled;
		bool bRetainEditorOnlyData = false;
		bool bDryRun = false;
		FCookCancellationCheck IsCancelled;
		FCookProgressCallback ReportProgress;
	};

	struct FCookRunResult
	{
		ECookRunStatus Status = ECookRunStatus::Failed;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::string Code;
		std::string Diagnostic;
		std::vector<FCookPackageResult> Packages;
		uint64 ChangedBytes = 0;
		uint64 ReusedBytes = 0;
		uint64 PeakCapturedBytes = 0;
		uint64 RangeReadCount = 0;
		uint64 WallTimeNanoseconds = 0;
		uint64 CommitTimeNanoseconds = 0;
		uint64 RollbackTimeNanoseconds = 0;
	};

	struct FCookStateEntry
	{
		std::string VirtualPackagePath;
		FXxHash128 InputFingerprint;
		FXxHash128 PackageDigest;
		FXxHash128 SegmentDigest;
		uint64 PackageSize = 0;
		uint64 SegmentSize = 0;
		uint32 ContributorVersion = 1;
		uint32 FamilyProducerVersion = 1;
		std::string Contributor;
		std::string BuildProvenance;
		uint8 SegmentFlags = 0;
	};

	struct FCookState
	{
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::vector<FCookStateEntry> Entries;
	};

	// Detached non-package output committed by the same manifest-last transaction.
	struct FCookAuxiliaryOutput
	{
		ECookManifestEntryKind Kind = ECookManifestEntryKind::ShaderLibrary;
		std::string RelativePath;
		FByteBuffer Bytes;
		FXxHash128 Digest;
	};

	ENGINE_API auto EncodeCookState(const FCookState& State, FByteBuffer& OutBytes, std::string* OutError = nullptr) -> bool;
	ENGINE_API auto DecodeCookState(FByteView Bytes, FCookState& OutState, std::string* OutError = nullptr) -> bool;

	using FCookFailureInjection = std::function<bool(
		ECookOperationStage, size_t, std::string&
	)>;

	// Classifies publication independently from its human-readable diagnostic.
	enum class ECookPublishStatus : uint8
	{
		Succeeded,
		Failed,
		Cancelled
	};

	struct FCookPublishResult
	{
		ECookPublishStatus Status = ECookPublishStatus::Failed;
		std::string Diagnostic;

		explicit operator bool() const
		{
			return Status == ECookPublishStatus::Succeeded;
		}
	};

	// Store transaction boundary; contributors never receive a store or path.
	class ICookOutputStore
	{
	public:
		virtual ~ICookOutputStore() = default;
		virtual auto Publish(std::span<const FCookSavePlan> Plans,
			std::span<const FCookAuxiliaryOutput> AuxiliaryOutputs,
			const FCookState& State,
			FCookRunResult& InOutResult,
			const FCookCancellationCheck& IsCancelled,
			const FCookFailureInjection& ShouldFail) -> FCookPublishResult = 0;
		auto Publish(std::span<const FCookSavePlan> Plans,
			const FCookState& State,
			FCookRunResult& InOutResult,
			const FCookCancellationCheck& IsCancelled,
			const FCookFailureInjection& ShouldFail) -> FCookPublishResult
		{
			return Publish(Plans, {}, State, InOutResult, IsCancelled, ShouldFail);
		}
	};

	ENGINE_API auto CreateLocalLooseCookOutputStore(
		std::filesystem::path OutputRoot,
		ECookTargetPlatform TargetPlatform,
		ECookTargetProfile TargetProfile
	) -> std::unique_ptr<ICookOutputStore>;

	class FCookContext
	{
	public:
		ENGINE_API FCookContext(
			std::filesystem::path InCookRoot,
			ECookTargetPlatform InTargetPlatform,
			ECookTargetProfile InTargetProfile,
			bool bInRetainEditorOnlyData = false
		);
		ENGINE_API auto AddPackage(
			std::string VirtualPackagePath,
			FByteBuffer PackageBytes,
			std::string* OutError = nullptr
		) -> bool;
		ENGINE_API auto AddPackage(
			std::string VirtualPackagePath,
			const FPackagePath& SourcePackagePath,
			FByteBuffer PackageBytes,
			std::string* OutError = nullptr
		) -> bool;
		ENGINE_API auto AddPackage(
			std::string VirtualPackagePath,
			DPackage* Package,
			std::string* OutError = nullptr
		) -> bool;
		// Publishes an opaque headerless raw segment owned by a higher-level
		// manifest rather than by reflected package fields.
		ENGINE_API auto AddRawPackage(
			std::string VirtualPackagePath,
			FByteBuffer PackageBytes,
			FByteBuffer RawSegmentBytes,
			std::string* OutError = nullptr
		) -> bool;
		ENGINE_API auto Publish(std::string* OutError = nullptr) -> bool;
		ENGINE_API auto TakeSavePlans(std::vector<FCookSavePlan>& OutPlans, std::string* OutError = nullptr) -> bool;
		auto GetSavePlans() const -> std::span<const FCookSavePlan> { return Packages; }
		auto GetTargetPlatform() const -> ECookTargetPlatform { return TargetPlatform; }
		auto GetTargetProfile() const -> ECookTargetProfile { return TargetProfile; }
		auto IsRetainingEditorOnlyData() const -> bool { return bRetainEditorOnlyData; }
		ENGINE_API auto MakePackageSerializationOptions() const
			-> FAssetPackageSerializationOptions;

	private:
		std::filesystem::path CookRoot;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		bool bRetainEditorOnlyData = false;
		std::vector<FCookSavePlan> Packages;
	};

	using FCookContributor = std::function<FAssetResult(
		DObject&, std::string_view, FCookContext&
	)>;
	using FCookContributorHandle = uint64;

	struct FCookContributorRegistration
	{
		std::string Name;
		uint32 ContributorVersion = 1;
		uint32 FamilyProducerVersion = 1;
		FCookContributor Contribute;
		std::function<ECookPackageStatus(const DObject&)> ClassifyPreparation;
	};

	ENGINE_API auto RegisterCookContributor(DClass* Class, FCookContributorRegistration Registration) -> FCookContributorHandle;
	ENGINE_API auto UnregisterCookContributor(FCookContributorHandle Handle) -> void;
	ENGINE_API auto RegisterEngineCookContributors(
		std::vector<FCookContributorHandle>& OutHandles,
		std::string& OutError) -> bool;
	ENGINE_API auto ContributeEngineCookAsset(
		DObject& Object,
		std::string_view VirtualPackagePath,
		FCookContext& Context,
		std::string& OutError) -> bool;

	// Runs one deterministic serial project Cook over a captured registry closure.
	class FCookCoordinator
	{
	public:
		ENGINE_API auto Run(const FCookRequest& Request, FCookRunResult& OutResult, ICookOutputStore* OutputStore = nullptr, FCookFailureInjection ShouldFail = {}) -> bool;
	};
} // namespace Durin
