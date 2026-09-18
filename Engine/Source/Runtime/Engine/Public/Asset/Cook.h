#pragma once

#include "Asset/AssetDefinitions.h"

#include "EngineAPI.h"
#include "Asset/CookedAsset.h"
#include "Asset/CookDependencies.h"
#include "Asset/PackageBulkData.h"
#include "DObject/DObjectFwd.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"

namespace Durin
{
	struct FAssetPackageSerializationOptions;
	struct FAssetPathResolveResult;
	struct FCookOutputRootResult;
	struct FCookPublishResult;
	struct FCookCaptureResult;
	struct FProjectGameSettingsResult;
	struct FShaderError;
	struct FObjectError;
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

	enum class ECookManifestError : uint8
	{
		None, Target, EntryCount, Entry, RecordLimit, Encoding, Truncated,
		Header, Checksum, Record, PathTruncated, TrailingBytes
	};
	struct FCookManifestResult
	{
		ECookManifestError Error = ECookManifestError::None;
		std::string RelativePath;
		uint64 Offset = 0;
		uint64 Actual = 0;
		uint64 Expected = 0;
		uint64 EntryIndex = 0;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		explicit operator bool() const { return Error == ECookManifestError::None; }
	};
	ENGINE_API auto FormatCookManifestError(const FCookManifestResult& Result) -> std::string;

	ENGINE_API auto EncodeCookManifest(
		const FCookManifest& Manifest,
		FByteBuffer& OutBytes
	) -> FCookManifestResult;
	ENGINE_API auto DecodeCookManifest(
		FByteView Bytes,
		FCookManifest& OutManifest
	) -> FCookManifestResult;

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
		ECookPackageStatus Status = ECookPackageStatus::Failed;
		ECookOperationStage Stage = ECookOperationStage::Discovery;
		uint64 PackageBytes = 0;
		uint64 SegmentBytes = 0;
	};

	ENGINE_API auto FormatCookPackageResult(const FCookPackageResult& Result) -> std::string;

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

	enum class ECookInputError : uint8
	{
		None, Cancelled, WriteConflict, FileIo, ByteLimit, PackageLimit, UnknownPackage,
		UndeclaredInput, NoReader, DeclarationCount, DeclarationName, DuplicateDeclaration,
		PackageDeclaration, ExternalDeclaration, ValueDeclaration, ValueStorage, ReservedKind,
		BulkIdentity, SchemaClass, SchemaDepth, SchemaFields, SchemaField, SchemaType, SchemaEncoding,
		SchemaStorage, RootLimit, RootClass, RuntimeEdgeLimit, ReferenceClass, NoRuntimePackages, DependencyStorage
	};
	struct FCookInputFailure
	{
		ECookInputError Error = ECookInputError::None;
		FPackagePath Package;
		ECookBuildDependencyKind Kind = ECookBuildDependencyKind::ExternalFile;
		std::string Name;
		std::filesystem::path File;
		uint64 Actual = 0;
		uint64 Maximum = 0;
		uint64 Retained = 0;
		uint64 MaximumRetained = 0;
		std::optional<FFileHelper::FFileIoError> FileCause;
		std::shared_ptr<const FObjectError> PathCause;
		std::string Member;
		uint64 Expected = 0;
		FXxHash128 ExpectedDigest;
		FXxHash128 ActualDigest;
		ENGINE_API auto ToAssetResult() const -> FAssetResult;
	};
	ENGINE_API auto FormatCookInputError(const FCookInputFailure& Failure) -> std::string;

	enum class ECookRunError : uint8
	{
		None, Unspecified,
		InvalidRequest,
		InvalidOutputRoot,
		WrongThread,
		CookInUse,
		Cancelled,
		ProjectSettingsFailed,
		InvalidDefaultLevel,
		InputFailed,
		LoadInjectedFailure,
		StaleRegistry,
		MissingTopLevelAsset,
		FingerprintFailed,
		OutputLimit,
		InvalidTopLevelAsset,
		PrepareInjectedFailure,
		MissingPackage,
		ContributionFailed,
		CaptureInjectedFailure,
		CaptureFailed,
		AuxiliaryInjectedFailure,
		ShaderLibraryFailed,
		PublicationFailed,
	};

	struct FCookRunInjectionCause
	{
		ECookOperationStage Stage = ECookOperationStage::Discovery;
		size_t Index = 0;
		// Failure-injection provider text, bounded to 2048 bytes when captured.
		std::string ExternalDiagnostic;
	};

	struct FCookRunResult
	{
		ECookRunStatus Status = ECookRunStatus::Failed;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		ECookRunError Error = ECookRunError::Unspecified;
		bool bDryRun = false;
		std::filesystem::path OutputRoot;
		FPackagePath CurrentPackage;
		std::string CurrentContributor;
		std::string AssetIdentity;
		ECookOperationStage Stage = ECookOperationStage::Discovery;
		uint64 RetainedOutputBytes = 0;
		uint64 RequestedPackageBytes = 0;
		uint64 RequestedBulkBytes = 0;
		uint64 MaximumOutputBytes = 0;
		std::optional<FCookDependencyCodecResult> FingerprintCause;
		std::optional<FCookRunInjectionCause> InjectionCause;
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
		std::optional<FCookInputFailure> InputDiagnostic;
		std::shared_ptr<const FCookPublishResult> PublicationCause;
		std::shared_ptr<const FCookOutputRootResult> OutputRootCause;
		std::shared_ptr<const FProjectGameSettingsResult> SettingsCause;
		std::shared_ptr<const FObjectError> DefaultLevelCause;
		std::shared_ptr<const FShaderError> ShaderCause;
		std::shared_ptr<const FCookCaptureResult> CaptureCause;
		std::shared_ptr<const FAssetResult> ContributionCause;
		FPackagePath ContributionPackage;
		std::string ContributionProvider;
		explicit operator bool() const { return Error == ECookRunError::None; }
	};

	ENGINE_API auto CookRunCodeName(const FCookRunResult& Result) -> std::string_view;
	ENGINE_API auto FormatCookRunError(const FCookRunResult& Result) -> std::string;

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

	enum class ECookStateError : uint8 { None, Header, DuplicatePath, String, ByteLimit, Entry, DependencyFrame, Dependency, TrailingBytes };
	struct FCookStateResult
	{
		ECookStateError Error = ECookStateError::None;
		std::string VirtualPath;
		uint64 EntryIndex = 0;
		uint64 Actual = 0;
		uint64 Maximum = 0;
		uint64 RemainingBytes = 0;
		uint32 Version = 0;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::optional<FCookDependencyCodecResult> DependencyCause;
		explicit operator bool() const { return Error == ECookStateError::None; }
	};
	ENGINE_API auto FormatCookStateError(const FCookStateResult& Result) -> std::string;
	ENGINE_API auto EncodeCookState(const FCookState& State, FByteBuffer& OutBytes) -> FCookStateResult;
	ENGINE_API auto DecodeCookState(FByteView Bytes, FCookState& OutState) -> FCookStateResult;

	using FCookFailureInjection = std::function<bool(
		ECookOperationStage, size_t, std::string&
	)>;

	// Publication disposition remains separate from its typed error.
	enum class ECookPublishStatus : uint8
	{
		Succeeded,
		Failed,
		Cancelled
	};

	enum class ECookPublishOperationError : uint8
	{
		CreateRoot, WriterLock, CreateTransaction, CancelledStaging, CancelledCommit,
		StageWrite, StageValidation, CommitDirectory, CommitBackup, CommitReplace
	};
	struct FCookPublishOperationFailure
	{
		ECookPublishOperationError Error = ECookPublishOperationError::CreateRoot;
		ECookOperationStage Stage = ECookOperationStage::Prepare;
		std::filesystem::path Path;
		std::error_code SystemError;
	};
	ENGINE_API auto FormatCookPublishOperationError(const FCookPublishOperationFailure& Failure) -> std::string;

	enum class ECookPublishValidationError : uint8
	{
		Request, Plan, OpaqueSegment, PackageIdentity, RawBulkClosure, AuxiliaryOutput
	};
	struct FCookPublishValidationFailure
	{
		ECookPublishValidationError Error = ECookPublishValidationError::Request;
		std::filesystem::path OutputRoot;
		std::string Path;
		uint64 Index = 0;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
	};
	ENGINE_API auto FormatCookPublishValidationError(const FCookPublishValidationFailure& Failure) -> std::string;

	enum class ECookPublishError : uint8 { None, Unspecified, Root, Path, Package, Manifest, State, Operation, Validation, Injected };
	struct FCookPublishInjectedFailure
	{
		ECookOperationStage Stage = ECookOperationStage::Prepare;
		uint64 Index = 0;
		// Opaque caller-provided injection text, bounded to 2048 bytes by the store.
		std::string ExternalDiagnostic;
	};
	struct FCookPublishResult
	{
		ECookPublishStatus Status = ECookPublishStatus::Failed;
		ECookPublishError Error = ECookPublishError::Unspecified;
		std::string VirtualPath;
		std::shared_ptr<const FCookOutputRootResult> RootCause;
		std::optional<FCookedPathResult> PathCause;
		std::shared_ptr<const FAssetResult> PackageCause;
		std::optional<FCookManifestResult> ManifestCause;
		std::optional<FCookStateResult> StateCause;
		std::optional<FCookPublishOperationFailure> OperationCause;
		std::optional<FCookPublishValidationFailure> ValidationCause;
		std::optional<FCookPublishInjectedFailure> InjectionCause;

		explicit operator bool() const
		{
			return Error == ECookPublishError::None;
		}
	};

	ENGINE_API auto FormatCookPublishError(const FCookPublishResult& Result) -> std::string;

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

	enum class ECookPlanError : uint8 { None, Path, SourceIdentity, EmptyPackage, DuplicatePath, InvalidPackage, Projection, EmptyRawPackage, SegmentLimit, Target, Canonicalization, Resolution, CanonicalDuplicate };
	struct FCookPlanError
	{
		ECookPlanError Code = ECookPlanError::None;
		std::string VirtualPath;
		std::string SourcePath;
		std::shared_ptr<const FAssetResult> ProjectionCause;
		uint64 PackageBytes = 0;
		uint64 SegmentBytes = 0;
		uint64 MaximumSegmentBytes = 0;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		std::shared_ptr<const FAssetResult> CanonicalizationCause;
		std::shared_ptr<const FAssetPathResolveResult> ResolutionCause;
	};
	struct FCookPlanResult
	{
		FCookPlanError Error;
		explicit operator bool() const { return Error.Code == ECookPlanError::None; }
	};
	ENGINE_API auto FormatCookPlanError(const FCookPlanError& Error) -> std::string;

	enum class ECookCaptureError : uint8 { None, Finalization, PlanCount };
	struct FCookCaptureResult
	{
		ECookCaptureError Error = ECookCaptureError::None;
		FPackagePath Package;
		std::string Contributor;
		uint64 ActualPlans = 0;
		uint64 ExpectedPlans = 1;
		std::optional<FCookPlanError> PlanCause;
		explicit operator bool() const { return Error == ECookCaptureError::None; }
	};
	ENGINE_API auto FormatCookCaptureError(const FCookCaptureResult& Result) -> std::string;


	enum class ECookContributionError : uint8
	{
		None, Target, PlatformData, RenderData, Revision, Contract, FunctionDependencies, UnsupportedClass, Plan,
		AuthoringOnly, TypeMismatch, SourceMutation, RecipeProvider, ShaderInputs
	};
	struct FCookContributionResult
	{
		ECookContributionError Error = ECookContributionError::None;
		std::string ObjectPath;
		std::string VirtualPath;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		uint64 AuthoredRevision = 0;
		std::optional<FCookPlanError> PlanCause;
		std::string ExpectedClass;
		std::string ActualClass;
		std::string Provider;
		explicit operator bool() const { return Error == ECookContributionError::None; }
		ENGINE_API auto ToAssetResult() const -> FAssetResult;
	};
	ENGINE_API auto FormatCookContributionError(const FCookContributionResult& Result) -> std::string;

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
			FByteBuffer PackageBytes
		) -> FCookPlanResult;
		ENGINE_API auto AddPackage(
			std::string VirtualPackagePath,
			const FPackagePath& SourcePackagePath,
			FByteBuffer PackageBytes
		) -> FCookPlanResult;
		ENGINE_API auto AddPackage(
			std::string VirtualPackagePath,
			DPackage* Package
		) -> FCookPlanResult;
		// Publishes an opaque headerless raw segment owned by a higher-level
		// manifest rather than by reflected package fields.
		ENGINE_API auto AddRawPackage(
			std::string VirtualPackagePath,
			FByteBuffer PackageBytes,
			FByteBuffer RawSegmentBytes
		) -> FCookPlanResult;
		// Consumes pending packages, including on failure; rebuild the context to retry.
		// OutPlans is empty on failure.
		ENGINE_API auto TakeSavePlans(std::vector<FCookSavePlan>& OutPlans) -> FCookPlanResult;
		using FReadInput = std::function<FAssetResult(ECookBuildDependencyKind, std::string_view, FByteBuffer&)>;
		auto SetInputReader(FReadInput Reader) -> void { ReadInput = std::move(Reader); }
		auto ReadDeclaredInput(ECookBuildDependencyKind Kind, std::string_view Name,
			FByteBuffer& Out) const -> FAssetResult {
			Out.clear();
			return ReadInput ? ReadInput(Kind, Name, Out)
				: FCookInputFailure{.Error = ECookInputError::NoReader, .Kind = Kind, .Name = std::string(Name)}.ToAssetResult();
		}
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

	enum class ECookContextPublishError : uint8 { None, Finalization, Publication };
	struct FCookContextPublishResult
	{
		ECookContextPublishError Error = ECookContextPublishError::None;
		std::filesystem::path OutputRoot;
		std::optional<FCookPlanError> PlanCause;
		std::optional<FCookPublishResult> PublicationCause;
		explicit operator bool() const { return Error == ECookContextPublishError::None; }
	};
	ENGINE_API auto FormatCookContextPublishError(const FCookContextPublishResult& Result) -> std::string;

	// Consumes a direct family context and publishes through the shared output store.
	// Production uses the coordinator for reachability, reuse and shader outputs.
	ENGINE_API auto PublishCookContext(FCookContext& Context,
		const std::filesystem::path& OutputRoot) -> FCookContextPublishResult;

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

	enum class ECookContributorRegistrationError : uint8 { None, InvalidClass, Name, Callback, Version, DuplicateClass };
	struct FCookContributorRegistrationResult
	{
		ECookContributorRegistrationError Error = ECookContributorRegistrationError::None;
		FCookContributorHandle Handle = 0;
		std::string ContributorName;
		std::string ClassName;
		uint32 ContributorVersion = 0;
		uint32 FamilyProducerVersion = 0;
		explicit operator bool() const { return Error == ECookContributorRegistrationError::None; }
	};
	ENGINE_API auto FormatCookContributorRegistrationError(const FCookContributorRegistrationResult& Result) -> std::string;
	ENGINE_API auto RegisterCookContributor(DClass* Class, FCookContributorRegistration Registration) -> FCookContributorRegistrationResult;
	// Retires from future runs without waiting; active runs retain the registration.
	// Callback destruction and owner release occur outside the registration mutex.
	ENGINE_API auto UnregisterCookContributor(FCookContributorHandle Handle) -> void;
	ENGINE_API auto RegisterEngineCookContributors(
		std::vector<FCookContributorHandle>& OutHandles) -> FCookContributorRegistrationResult;
	ENGINE_API auto ContributeEngineCookAsset(
		DObject& Object,
		std::string_view VirtualPackagePath,
		FCookContext& Context) -> FCookContributionResult;

	enum class ECookOutputRootError : uint8
	{
		None, AbsolutePath, CanonicalizeOutput, CanonicalizeInput, AuthoredOverlap,
		InspectTree, EnumerateTree, InspectEntry, ResolveAlias, EntryLimit, AliasEscape
	};
	struct FCookOutputRootResult
	{
		ECookOutputRootError Error = ECookOutputRootError::None;
		std::filesystem::path OutputRoot;
		std::filesystem::path RelatedPath;
		std::filesystem::path ResolvedPath;
		std::error_code SystemError;
		uint64 Entries = 0;
		uint64 MaximumEntries = 0;
		explicit operator bool() const { return Error == ECookOutputRootError::None; }
	};
	ENGINE_API auto FormatCookOutputRootError(const FCookOutputRootResult& Result) -> std::string;

	// Validates the write destination against mounted authored trees, resolving existing
	// filesystem aliases. Performs no writes. Hosts call this before starting services.
	ENGINE_API auto ValidateCookOutputRoot(const std::filesystem::path& OutputRoot) -> FCookOutputRootResult;

	// Internal owner-thread orchestration. Production callers launch DurinAssetTool cook
	// once against stable saved inputs; live-editor use is unsupported.
	class FCookCoordinator
	{
	public:
		ENGINE_API auto Run(const FCookRequest& Request, FCookRunResult& OutResult, ICookOutputStore* OutputStore = nullptr, FCookFailureInjection ShouldFail = {}) -> bool;
	};
} // namespace Durin
