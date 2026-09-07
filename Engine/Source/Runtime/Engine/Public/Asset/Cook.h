#pragma once

#include "Asset/AssetDefinitions.h"

#include "EngineAPI.h"
#include "Asset/CookedAsset.h"
#include "Asset/CookDependencies.h"
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

	enum class ECookInputStatus : uint8
	{
		None, ProjectionPending,
		UndeclaredInput, InvalidDependency, LimitExceeded, IoError, Cancelled
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
		// Accounted retained dependency values plus detached outputs; excludes process RSS,
		// transient codec/compiler buffers, and ordinary loader/resource allocations.
		uint64 PeakRetainedBytes = 0;
		uint64 WallTimeNanoseconds = 0;
		uint64 CommitTimeNanoseconds = 0;
		uint64 RollbackTimeNanoseconds = 0;
		ECookInputStatus InputStatus = ECookInputStatus::None;
		FAssetResult InputFailure;
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
		std::vector<FCookBuildDependency> BuildDependencies;
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

	// Builds detached package plans without owning a disk publication destination.
	class FCookContext
	{
	public:
		ENGINE_API FCookContext(
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
		// Consumes pending packages, including on failure; rebuild the context to retry.
		// OutPlans is empty on failure.
		ENGINE_API auto TakeSavePlans(std::vector<FCookSavePlan>& OutPlans, std::string* OutError = nullptr) -> bool;
		using FReadInput = std::function<FAssetResult(ECookBuildDependencyKind, std::string_view, FByteBuffer&)>;
		auto SetInputReader(FReadInput Reader) -> void { ReadInput = std::move(Reader); }
		auto ReadDeclaredInput(ECookBuildDependencyKind Kind, std::string_view Name,
			FByteBuffer& Out) const -> FAssetResult { return ReadInput ? ReadInput(Kind, Name, Out)
				: FAssetResult{EAssetError::MissingDependency, "No declared Cook inputs."}; }
		auto GetSavePlans() const -> std::span<const FCookSavePlan> { return Packages; }
		auto GetTargetPlatform() const -> ECookTargetPlatform { return TargetPlatform; }
		auto GetTargetProfile() const -> ECookTargetProfile { return TargetProfile; }
		auto IsRetainingEditorOnlyData() const -> bool { return bRetainEditorOnlyData; }
		ENGINE_API auto MakePackageSerializationOptions() const
			-> FAssetPackageSerializationOptions;

	private:
		FReadInput ReadInput;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		bool bRetainEditorOnlyData = false;
		std::vector<FCookSavePlan> Packages;
	};

	// Consumes a direct family context and publishes through the shared output store.
	// Production uses the coordinator for reachability, reuse and shader outputs.
	ENGINE_API auto PublishCookContext(FCookContext& Context,
		const std::filesystem::path& OutputRoot, std::string* OutError = nullptr) -> bool;

	using FCookContributor = std::function<FAssetResult(
		DObject&, std::string_view, FCookContext&
	)>;
	using FCookContributorHandle = uint64;

	// Describes contributor callbacks and the owner retained by active runs.
	struct FCookContributorRegistration
	{
		std::string Name;
		uint32 ContributorVersion = 1;
		uint32 FamilyProducerVersion = 1;
		FCookContributor Contribute;
		std::function<ECookPackageStatus(const DObject&)> ClassifyPreparation;
		// Pins callback state and native code until the last active run releases
		// this registration. Empty means the caller guarantees process lifetime.
		std::shared_ptr<void> LifetimeOwner;
		// Absence disables incremental reuse. Native callbacks must declare every
		// external input; automatic source/bulk/hard dependencies are added by Cook.
		FCookDependencyDeclarationCallback DeclareDependencies;

		// Callback managers may execute provider code during destruction.
		~FCookContributorRegistration()
		{
			DeclareDependencies = {};
			ClassifyPreparation = {};
			Contribute = {};
		}
	};

	ENGINE_API auto RegisterCookContributor(DClass* Class, FCookContributorRegistration Registration) -> FCookContributorHandle;
	// Retires from future runs without waiting; active runs retain the registration.
	// Callback destruction and owner release occur outside the registration mutex.
	ENGINE_API auto UnregisterCookContributor(FCookContributorHandle Handle) -> void;
	ENGINE_API auto RegisterEngineCookContributors(
		std::vector<FCookContributorHandle>& OutHandles,
		std::string& OutError) -> bool;
	ENGINE_API auto ContributeEngineCookAsset(
		DObject& Object,
		std::string_view VirtualPackagePath,
		FCookContext& Context,
		std::string& OutError) -> bool;

	// Validates the write destination against mounted authored trees, resolving existing
	// filesystem aliases. Performs no writes. Hosts call this before starting services.
	ENGINE_API auto ValidateCookOutputRoot(const std::filesystem::path& OutputRoot,
		std::string& OutError) -> bool;

	// Internal owner-thread orchestration. Production callers launch DurinAssetTool cook
	// once against stable saved inputs; live-editor use is unsupported.
	class FCookCoordinator
	{
	public:
		ENGINE_API auto Run(const FCookRequest& Request, FCookRunResult& OutResult, ICookOutputStore* OutputStore = nullptr, FCookFailureInjection ShouldFail = {}) -> bool;
	};
} // namespace Durin
