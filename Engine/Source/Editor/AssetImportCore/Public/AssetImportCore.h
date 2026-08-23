#pragma once

#include "Asset/PackageAuthoring.h"
#include "Asset/SourcePath.h"
#include "AssetImportCoreAPI.h"
#include "DObject/AssetPath.h"
#include "Hash/XxHash.h"
#include "Modules/ModularFeature.h"

namespace Durin::Asset
{
	enum class EImportDiagnosticSeverity : uint8
	{
		Info,
		Warning,
		Error
	};

	enum class EImportDiagnosticCategory : uint8
	{
		InvalidRequest,
		ProviderUnavailable,
		ProviderAmbiguous,
		ProviderFailure,
		CapabilityUnavailable,
		CandidateFailure,
		ValidationFailure,
		PublicationFailure,
		RestoreFailure,
		InvalidSource,
		MissingDependency,
		UnsafeDependency,
		DuplicateSource,
		DependencyCycle,
		ResourceLimitExceeded,
		InvalidPlan,
		Collision,
		StalePlan,
		Canceled,
		AsyncFailure
	};

	struct FImportDiagnostic
	{
		EImportDiagnosticSeverity Severity = EImportDiagnosticSeverity::Error;
		EImportDiagnosticCategory Category = EImportDiagnosticCategory::InvalidRequest;
		// Providers may supply a stable identity. The framework derives one from
		// the stable context when this is empty before preview or persistence.
		std::string Identity;
		std::string Phase;
		std::string SourceIdentity;
		std::string OutputIdentity;
		std::string Message;

		auto operator==(const FImportDiagnostic&) const -> bool = default;
	};

	enum class EImportPhase : uint8
	{
		Snapshot,
		Parse,
		Plan,
		CandidateBuild,
		Validation,
		Publication,
		Restore
	};

	enum class EImportProgressState : uint8
	{
		Started,
		Succeeded,
		Failed
	};

	struct FImportProgressEvent
	{
		EImportPhase Phase = EImportPhase::Snapshot;
		EImportProgressState State = EImportProgressState::Started;
		std::string SourceIdentity;
		std::string OutputIdentity;
		uint64 CompletedWork = 0;
		uint64 TotalWork = 0;
		std::string Message;

		auto operator==(const FImportProgressEvent&) const -> bool = default;
	};

	class ASSETIMPORTCORE_API IImportProgressReporter
	{
	public:
		virtual ~IImportProgressReporter() = default;
		virtual auto Report(const FImportProgressEvent& Event) noexcept -> void = 0;
	};

	ASSETIMPORTCORE_API auto GetImportDiagnosticIdentity(
		const FImportDiagnostic& Diagnostic) -> std::string;
	ASSETIMPORTCORE_API auto FinalizeImportDiagnostics(
		std::vector<FImportDiagnostic>& Diagnostics,
		std::string_view DefaultPhase,
		std::string_view DefaultSourceIdentity = "root",
		std::string_view DefaultOutputIdentity = "request") -> void;
	ASSETIMPORTCORE_API auto ReportImportProgress(
		IImportProgressReporter* Reporter,
		EImportPhase Phase,
		EImportProgressState State,
		std::string_view SourceIdentity = "root",
		std::string_view OutputIdentity = "request",
		uint64 CompletedWork = 0,
		uint64 TotalWork = 0,
		std::string_view Message = {}) noexcept -> void;

	struct FImportSourcePreview
	{
		std::string StableIdentity;
		std::string Role;
		FSourcePath SourcePath;
		uint64 ByteCount = 0;
		bool bEmbedded = false;

		auto operator==(const FImportSourcePreview&) const -> bool = default;
	};

	struct FImportPayload
	{
		std::string SchemaId;
		uint32 SchemaVersion = 0;
		std::vector<std::byte> Bytes;
		FXxHash128 ContentHash{};

		ASSETIMPORTCORE_API auto Finalize(std::string& OutError) -> bool;
		auto operator==(const FImportPayload&) const -> bool = default;
	};

	struct FSourceCaptureLimits
	{
		uint32 MaximumDependencyDepth = 32;
		uint32 MaximumSourceCount = 8'192;
		uint64 MaximumBytesPerSource = 2ull * 1024ull * 1024ull * 1024ull;
		uint64 MaximumAggregateBytes = 4ull * 1024ull * 1024ull * 1024ull;
		uint64 MaximumEmbeddedBytes = 2ull * 1024ull * 1024ull * 1024ull;
		uint64 MaximumSettingsBytes = 4ull * 1024ull * 1024ull;
		uint64 RecognitionPrefixBytes = 64ull * 1024ull;
	};

	struct FSourceSnapshotEntry
	{
		std::string StableIdentity;
		std::string Role;
		FSourcePath SourcePath;
		std::string DeclaringIdentity;
		FXxHash128 ContentHash{};
		uint64 ByteCount = 0;
		uint32 Depth = 0;
		bool bEmbedded = false;

		ASSETIMPORTCORE_API auto GetBytes() const -> std::span<const std::byte>;

		// Shared immutable storage permits several logical identities to reference
		// one physical capture without reopening or duplicating its bytes.
		std::shared_ptr<const std::vector<std::byte>> Bytes;
	};

	class ASSETIMPORTCORE_API FSourceSnapshot
	{
	public:
		auto GetSources() const -> std::span<const FSourceSnapshotEntry> { return Sources; }
		auto GetAggregateByteCount() const -> uint64 { return AggregateByteCount; }
		auto FindSource(std::string_view StableIdentity) const -> const FSourceSnapshotEntry*;

	private:
		std::vector<FSourceSnapshotEntry> Sources;
		uint64 AggregateByteCount = 0;

		friend class FSourceSnapshotBuilder;
	};

	struct FDependencyRequest
	{
		std::string DeclaringIdentity;
		std::string StableIdentity;
		std::string Role;
		std::string RelativePath;
		std::vector<std::byte> EmbeddedBytes;
		bool bOptional = false;

		auto IsEmbedded() const -> bool { return !EmbeddedBytes.empty(); }
	};

	class ASSETIMPORTCORE_API FDependencyRequestSink
	{
	public:
		auto AddRelative(
			std::string_view DeclaringIdentity,
			std::string_view StableIdentity,
			std::string_view Role,
			std::string_view RelativePath,
			bool bOptional = false) -> bool;
		auto AddEmbedded(
			std::string_view DeclaringIdentity,
			std::string_view StableIdentity,
			std::string_view Role,
			std::span<const std::byte> Bytes) -> bool;

	private:
		explicit FDependencyRequestSink(
			std::vector<FDependencyRequest>& InRequests,
			std::vector<FImportDiagnostic>& InDiagnostics,
			uint32 InMaximumRequests,
			uint64 InMaximumBytesPerSource,
			uint64 InMaximumEmbeddedBytes)
			: Requests(InRequests)
			, Diagnostics(InDiagnostics)
			, MaximumRequests(InMaximumRequests)
			, MaximumBytesPerSource(InMaximumBytesPerSource)
			, MaximumEmbeddedBytes(InMaximumEmbeddedBytes) {}

		std::vector<FDependencyRequest>& Requests;
		std::vector<FImportDiagnostic>& Diagnostics;
		uint32 MaximumRequests = 0;
		uint64 MaximumBytesPerSource = 0;
		uint64 MaximumEmbeddedBytes = 0;
		uint64 RequestedEmbeddedBytes = 0;

		friend class FSourceSnapshotBuilder;
	};

	struct FImportSourceRecognition
	{
		FSourcePath RootSource;
		std::string Extension;
		uint64 ByteCount = 0;
		std::span<const std::byte> Prefix;
	};

	enum class EImportOutputPolicy : uint8
	{
		Create,
		ReplaceWholeState,
		ReplaceTypedSubobjects,
		Reference,
		Detach
	};

	enum class EImportCollisionAction : uint8
	{
		None,
		Create,
		ReplaceManaged,
		RejectUnrelated,
		RequireExplicitRepair
	};

	struct FImportOutputPreview
	{
		std::string StableIdentity;
		std::string Role;
		FAssetPath AssetPath;
		std::string AssetClassName;
		EImportOutputPolicy Policy = EImportOutputPolicy::Create;
		EImportCollisionAction Collision = EImportCollisionAction::None;
		uint64 EstimatedCpuBytes = 0;
		uint64 EstimatedGpuBytes = 0;
		uint64 EstimatedDiskBytes = 0;

		auto operator==(const FImportOutputPreview&) const -> bool = default;
	};

	enum class EImportPreviewAction : uint8
	{
		Create,
		Replace,
		Reference,
		KeepDetached,
		Missing,
		Collision,
		Orphan
	};

	struct FImportPreviewOutput
	{
		FImportOutputPreview Output;
		EImportPreviewAction Action = EImportPreviewAction::Create;
		bool bManaged = true;

		auto operator==(const FImportPreviewOutput&) const -> bool = default;
	};

	enum class EImportWarningChange : uint8
	{
		New,
		PreviouslyAccepted,
		Resolved
	};

	struct FImportWarningPreview
	{
		EImportWarningChange Change = EImportWarningChange::New;
		FImportDiagnostic Diagnostic;

		auto operator==(const FImportWarningPreview&) const -> bool = default;
	};

	struct FImportPreview
	{
		std::vector<FImportSourcePreview> Sources;
		std::vector<FImportPreviewOutput> Outputs;
		std::vector<FImportWarningPreview> Warnings;
		uint64 EstimatedCpuBytes = 0;
		uint64 EstimatedGpuBytes = 0;
		uint64 EstimatedDiskBytes = 0;
	};

	struct FImportTargetPrecondition
	{
		FAssetPath AssetPath;
		std::string AssetClassName;
		uint64 PackageEditRevision = 0;
		std::string AuthoredFingerprint;
		FAssetPath ManagementOwner;

		auto operator==(const FImportTargetPrecondition&) const -> bool = default;
	};

	class FProviderLease;
	class FImporterStore;
	class FImportPlanBuilder;
	struct FImportPlanResult;
	struct FImportPlanRequest;

	class ASSETIMPORTCORE_API IImportProvider
	{
	public:
		virtual ~IImportProvider() = default;
		virtual auto GetProviderId() const -> std::string_view = 0;
		virtual auto GetContractVersion() const -> uint32 = 0;
		virtual auto CanImport(const FImportSourceRecognition& Source) const -> bool = 0;
		virtual auto CaptureSettings(
			FImportPayload& OutSettings,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
		virtual auto DiscoverDependencies(
			std::span<const FSourceSnapshotEntry> Sources,
			FDependencyRequestSink& Sink,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
		virtual auto Plan(
			const FSourceSnapshot& Snapshot,
			const FImportPayload& Settings,
			FImportPlanBuilder& Builder,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
	};

	struct FProviderLeaseState;

	class ASSETIMPORTCORE_API FProviderLease
	{
	public:
		FProviderLease();
		~FProviderLease();
		FProviderLease(const FProviderLease&);
		FProviderLease(FProviderLease&&) noexcept;
		auto operator=(const FProviderLease&) -> FProviderLease&;
		auto operator=(FProviderLease&&) noexcept -> FProviderLease&;

		auto IsValid() const -> bool { return State != nullptr; }
		explicit operator bool() const { return IsValid(); }
		auto GetProvider() const -> const IImportProvider*;
		auto GetProviderId() const -> std::string_view;
		auto GetContractVersion() const -> uint32;
		auto TryEnter() const -> FModuleOwnedCallbackInvocation;

	private:
		explicit FProviderLease(std::shared_ptr<const FProviderLeaseState> InState)
			: State(std::move(InState)) {}

		std::shared_ptr<const FProviderLeaseState> State;
		std::shared_ptr<FModuleOwnedResourceLease> ResourceLease;

		friend class FImporterStore;
	};

	// Serializes only final import preflight and authored-package publication.
	ASSETIMPORTCORE_API auto GetImportPublicationMutex() -> std::mutex&;
	ASSETIMPORTCORE_API auto BuildImportPlan(
		const FProviderLease& Provider,
		std::shared_ptr<const FSourceSnapshot> Snapshot,
		const FImportPayload& Settings,
		uint64 ImporterRevision,
		std::span<const FImportDiagnostic> PriorDiagnostics = {},
		IImportProgressReporter* Progress = nullptr) -> FImportPlanResult;

	class ASSETIMPORTCORE_API FSourceSnapshotBuilder
	{
	public:
		explicit FSourceSnapshotBuilder(FSourceCaptureLimits InLimits = {});
		~FSourceSnapshotBuilder();
		FSourceSnapshotBuilder(FSourceSnapshotBuilder&&) noexcept;
		auto operator=(FSourceSnapshotBuilder&&) noexcept -> FSourceSnapshotBuilder&;
		FSourceSnapshotBuilder(const FSourceSnapshotBuilder&) = delete;
		auto operator=(const FSourceSnapshotBuilder&) -> FSourceSnapshotBuilder& = delete;

		auto CaptureRoot(
			const FSourcePath& RootSource,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
		// Captures caller-owned immutable bytes under a logical mounted identity.
		// This supports explicit source ingestion workflows without reopening the
		// external authoring file during provider planning.
		auto CaptureRootBytes(
			const FSourcePath& RootSource,
			std::span<const std::byte> Bytes,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
		auto CaptureDeclaredSource(
			std::string_view StableIdentity,
			std::string_view Role,
			const FSourcePath& Source,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
		auto CaptureDeclaredBytes(
			std::string_view StableIdentity,
			std::string_view Role,
			const FSourcePath& Source,
			std::span<const std::byte> Bytes,
			bool bEmbedded,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
		auto DiscoverDependencies(
			const FProviderLease& Provider,
			std::vector<FImportDiagnostic>& OutDiagnostics) -> bool;
		auto Freeze(
			std::vector<FImportDiagnostic>& OutDiagnostics) -> std::shared_ptr<const FSourceSnapshot>;
		auto GetCapturedSources() const -> std::span<const FSourceSnapshotEntry>;
		auto GetLimits() const -> const FSourceCaptureLimits&;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	class ASSETIMPORTCORE_API FImportPlanBuilder
	{
	public:
		auto AddOutput(FImportOutputPreview Output) -> void;
		auto AddTargetPrecondition(FImportTargetPrecondition Precondition) -> void;

		template<typename T>
		auto SetProviderData(std::shared_ptr<const T> Data) -> void
		{
			ProviderData = std::move(Data);
		}

	private:
		std::vector<FImportOutputPreview> Outputs;
		std::vector<FImportTargetPrecondition> TargetPreconditions;
		std::shared_ptr<const void> ProviderData;

		friend ASSETIMPORTCORE_API auto BuildImportPlan(
			const FProviderLease&,
			std::shared_ptr<const FSourceSnapshot>,
			const FImportPayload&,
			uint64,
			std::span<const FImportDiagnostic>,
			IImportProgressReporter*) -> FImportPlanResult;
	};

	class ASSETIMPORTCORE_API FImportPlan
	{
	public:
		FImportPlan() = default;
		auto GetProvider() const -> const FProviderLease& { return Provider; }
		auto GetSnapshot() const -> const FSourceSnapshot& { return *Snapshot; }
		auto GetSettings() const -> const FImportPayload& { return Settings; }
		auto GetOutputs() const -> std::span<const FImportOutputPreview> { return Outputs; }
		auto GetTargetPreconditions() const -> std::span<const FImportTargetPrecondition> { return TargetPreconditions; }
		auto GetDiagnostics() const -> std::span<const FImportDiagnostic> { return Diagnostics; }
		auto GetFingerprint() const -> const FXxHash128& { return Fingerprint; }
		auto GetProviderData() const -> const std::shared_ptr<const void>& { return ProviderData; }
		auto GetImporterRevision() const -> uint64 { return ImporterRevision; }

	private:
		FProviderLease Provider;
		std::shared_ptr<const FSourceSnapshot> Snapshot;
		FImportPayload Settings;
		std::vector<FImportOutputPreview> Outputs;
		std::vector<FImportTargetPrecondition> TargetPreconditions;
		std::vector<FImportDiagnostic> Diagnostics;
		std::shared_ptr<const void> ProviderData;
		FXxHash128 Fingerprint{};
		uint64 ImporterRevision = 0;

		friend ASSETIMPORTCORE_API auto BuildImportPlan(
			const FProviderLease&,
			std::shared_ptr<const FSourceSnapshot>,
			const FImportPayload&,
			uint64,
			std::span<const FImportDiagnostic>,
			IImportProgressReporter*) -> FImportPlanResult;
		friend class FImportService;
	};

	struct FImportPlanRequest
	{
		FSourcePath RootSource;
		std::string ProviderId;
		FSourceCaptureLimits Limits;
		// When present, the caller has already captured and normalized the
		// provider settings. This value is copied into asynchronous request state.
		std::optional<FImportPayload> Settings;
		IImportProgressReporter* Progress = nullptr;
	};

	struct FImportPlanResult
	{
		bool bSucceeded = false;
		std::string Message;
		FImportPlan Plan;
		std::vector<FImportDiagnostic> Diagnostics;

		explicit operator bool() const { return bSucceeded; }
	};

	// A provider-neutral projection of the lightweight provenance retained by a
	// concrete runtime asset. Settings remain opaque to the framework while the
	// source vocabulary and provider identity stay common across asset types.
	struct FSingleAssetSourceProvenance
	{
		std::string StableIdentity;
		std::string Role;
		FSourcePath SourcePath;
		FXxHash128 ContentHash{};
		uint64 ByteCount = 0;

		auto operator==(const FSingleAssetSourceProvenance&) const -> bool = default;
	};

	struct FSingleAssetProvenance
	{
		std::string ProviderId;
		uint32 ProviderContractVersion = 0;
		FImportPayload Settings;
		std::vector<FSingleAssetSourceProvenance> Sources;
		std::string AuthoredOutputFingerprint;

		auto IsComplete() const -> bool
		{
			return !ProviderId.empty() && ProviderContractVersion != 0
				&& !Settings.SchemaId.empty() && Settings.SchemaVersion != 0
				&& !Sources.empty();
		}
		auto operator==(const FSingleAssetProvenance&) const -> bool = default;
	};

	enum class ESingleAssetImportCapability : uint8
	{
		Import,
		ReimportCurrentSource,
		ReimportNewSource,
		RepairSource
	};

	struct FSingleAssetCapability
	{
		ESingleAssetImportCapability Capability = ESingleAssetImportCapability::Import;
		bool bAvailable = false;
		std::string Label;
		std::string ReplacedStateDescription;
		std::vector<FImportDiagnostic> Diagnostics;
	};

	struct FSingleAssetCapabilitySet
	{
		std::string AssetClassName;
		std::string ProviderId;
		std::vector<FSingleAssetCapability> Capabilities;

		auto Find(ESingleAssetImportCapability Capability) const -> const FSingleAssetCapability*
		{
			const auto It = std::ranges::find(
				Capabilities, Capability, &FSingleAssetCapability::Capability);
			return It == Capabilities.end() ? nullptr : &*It;
		}
	};

	class FSingleAssetImportPlan;
	class FImportService;
	struct FSingleAssetPlanResult;
	struct FSingleAssetExecutionOptions;
	struct FSingleAssetExecutionResult;

	class ASSETIMPORTCORE_API ISingleAssetCandidate
	{
	public:
		virtual ~ISingleAssetCandidate() = default;
		virtual auto GetAsset() const -> DObject* = 0;
		virtual auto GetPackage() const -> DPackage* = 0;
		virtual auto IsNewAsset() const -> bool = 0;
		virtual auto GetAuthoredFingerprint() const -> std::string = 0;
		virtual auto Validate(std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
		virtual auto Abandon() noexcept -> void = 0;
	};

	// All failable runtime work is completed before this token is returned.
	// Commit and Reverse are deliberately no-fail and symmetric.
	class ASSETIMPORTCORE_API IPreparedImportedStateExchange
	{
	public:
		virtual ~IPreparedImportedStateExchange() = default;
		virtual auto Commit() noexcept -> void = 0;
		virtual auto Reverse() noexcept -> void = 0;
		virtual auto Finalize() noexcept -> void = 0;
	};

	class ASSETIMPORTCORE_API ISingleAssetImportHandler
	{
	public:
		virtual ~ISingleAssetImportHandler() = default;
		virtual auto GetAssetClassName() const -> std::string_view = 0;
		virtual auto GetProviderId() const -> std::string_view = 0;
		virtual auto InspectProvenance(
			const DObject& Asset,
			FSingleAssetProvenance& OutProvenance,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
		virtual auto QueryCapabilities(
			const DObject& Asset,
			const FSingleAssetProvenance& Provenance) const -> FSingleAssetCapabilitySet = 0;
		virtual auto BuildCandidate(
			const FSingleAssetImportPlan& Plan,
			std::vector<FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<ISingleAssetCandidate> = 0;
		virtual auto PrepareImportedStateExchange(
			DObject& Target,
			ISingleAssetCandidate& Candidate,
			std::vector<FImportDiagnostic>& OutDiagnostics) const
			-> std::unique_ptr<IPreparedImportedStateExchange> = 0;
		virtual auto RepairSource(
			DObject& Asset,
			std::span<const FSourcePath> Sources,
			std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool = 0;
	};

	struct FSingleAssetReimportRequest
	{
		DObject* Asset = nullptr;
		// Empty reuses persisted sources. A populated list performs a mounted,
		// no-copy source replacement and must match the provider's source arity.
		std::vector<FSourcePath> ReplacementSources;
		FSourceCaptureLimits Limits;
		IImportProgressReporter* Progress = nullptr;
	};

	class ASSETIMPORTCORE_API FSingleAssetImportPlan
	{
	public:
		auto GetAsset() const -> DObject* { return Asset; }
		auto GetAssetPath() const -> const FAssetPath& { return AssetPath; }
		auto GetAssetClassName() const -> std::string_view { return AssetClassName; }
		auto GetProvenance() const -> const FSingleAssetProvenance& { return Provenance; }
		auto GetSnapshot() const -> const FSourceSnapshot& { return *Snapshot; }
		auto GetProvider() const -> const FProviderLease& { return Provider; }
		auto GetPackageEditRevision() const -> uint64 { return PackageEditRevision; }
		auto GetServiceRevision() const -> uint64 { return ServiceRevision; }
		auto ReplacesSource() const -> bool { return bReplacesSource; }

	private:
		DObject* Asset = nullptr;
		FAssetPath AssetPath;
		std::string AssetClassName;
		FSingleAssetProvenance Provenance;
		FSingleAssetProvenance ObservedProvenance;
		std::shared_ptr<const FSourceSnapshot> Snapshot;
		FProviderLease Provider;
		std::shared_ptr<const ISingleAssetImportHandler> Handler;
		FImportService* Service = nullptr;
		uint64 PackageEditRevision = 0;
		uint64 ServiceRevision = 0;
		bool bReplacesSource = false;

		friend class FImportService;
	};

	struct FSingleAssetPlanResult
	{
		bool bSucceeded = false;
		std::string Message;
		FSingleAssetImportPlan Plan;
		std::vector<FImportDiagnostic> Diagnostics;

		explicit operator bool() const { return bSucceeded; }
	};

	struct FSingleAssetExecutionOptions
	{
		Asset::FAssetBundleSaveOptions SaveOptions;
		IImportProgressReporter* Progress = nullptr;
	};

	struct FSingleAssetExecutionResult
	{
		bool bSucceeded = false;
		std::string Message;
		DObject* Asset = nullptr;
		std::vector<FImportDiagnostic> Diagnostics;
		FProviderLease Provider;

		explicit operator bool() const { return bSucceeded; }
	};

}
