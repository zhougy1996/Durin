#pragma once

#include "AssetCoreAPI.h"
#include "CookedAsset.h"
#include "Delegates/Delegate.h"
#include "DObject/CoreDObject.h"
#include "Hash/XxHash.h"

namespace Durin::Asset
{
	class DAssetRedirector;

	// Classifies failures returned by asset storage and registry operations.
	enum class EAssetError : uint8
	{
		None,
		InvalidPath,
		AlreadyExists,
		NotFound,
		IoError,
		CorruptFile,
		UnsupportedVersion,
		UnknownClass,
		TypeMismatch,
		MissingDependency,
		CircularDependency,
		InvalidObjectGraph,
		UnsupportedProperty,
		InvalidPackageType,
		InUse,
		StaleData,
		ReadOnlyMode,
		ShuttingDown
	};

	// Returns an asset operation status with an optional diagnostic message.
	struct FAssetResult
	{
		EAssetError Error = EAssetError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetError::None; }
		explicit operator bool() const { return Succeeded(); }
	};

	enum class ESoftObjectNullPolicy : uint8
	{
		Reject,
		Allow
	};

	enum class ESoftObjectResolveState : uint8
	{
		Null,
		NotLoaded,
		Loaded
	};

	struct FSoftObjectResolveResult
	{
		FAssetResult Result;
		ESoftObjectResolveState State = ESoftObjectResolveState::Null;
		DObject* Object = nullptr;
		FAssetPath ResolvedPath;
		bool bRedirected = false;

		auto Succeeded() const -> bool { return Result.Succeeded(); }
		explicit operator bool() const { return Succeeded(); }
	};

	template<typename T>
	struct TSoftObjectResolveResult
	{
		FAssetResult Result;
		ESoftObjectResolveState State = ESoftObjectResolveState::Null;
		T* Object = nullptr;
		FAssetPath ResolvedPath;
		bool bRedirected = false;

		auto Succeeded() const -> bool { return Result.Succeeded(); }
		explicit operator bool() const { return Succeeded(); }
	};

	// Classifies how a serialized field mismatch was handled during package loading.
	enum class EAssetCompatibilityClassification : uint8
	{
		SafeCleanup,
		Migrated,
		DataLossRisk,
		UnknownIncompatible
	};

	// Describes the consequence of persisting the in-memory representation after loading.
	enum class EAssetCompatibilityRisk : uint8
	{
		None,
		PotentialDataLoss,
		UnknownNewerSchema
	};

	// Preserves one incompatible serialized field and its original payload for a registered upgrader.
	struct FAssetLegacyField
	{
		std::string DeclaringClass;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		std::vector<uint8> Payload;
		// DAST v4 provenance 02 retains these two spans exactly. V2/v3 legacy
		// records leave them empty and continue to use Payload alone.
		std::vector<uint8> DescriptorClosure;
		std::vector<uint8> RetainedPayload;
	};

	// Groups related legacy fields on one object into a single user-facing compatibility item.
	struct FAssetCompatibilityIssue
	{
		std::string ObjectPath;
		std::string DeclaringClass;
		std::vector<FAssetLegacyField> LegacyFields;
		EAssetCompatibilityClassification Classification = EAssetCompatibilityClassification::UnknownIncompatible;
		std::string MigrationSummary;
		uint64 MigratedDataCount = 0;
		EAssetCompatibilityRisk Risk = EAssetCompatibilityRisk::UnknownNewerSchema;
		std::string HandlerId;
	};

	enum class EAssetLoadMutationKind : uint8
	{
		Upgrade,
		NonUpgrade
	};

	// Records an authored-state change made while materializing a package.
	struct FAssetLoadMutation
	{
		FAssetPath PackagePath;
		std::string ObjectPath;
		std::string HandlerId;
		std::string Summary;
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade;
	};

	// Carries structured compatibility results for one loaded package.
	struct FAssetLoadReport
	{
		FAssetPath PackagePath;
		std::vector<FAssetCompatibilityIssue> CompatibilityIssues;
		std::vector<FAssetLoadMutation> Mutations;

		auto HasCompatibilityIssues() const -> bool { return !CompatibilityIssues.empty(); }
		ASSETCORE_API auto HasNonUpgradeMutations() const -> bool;
		ASSETCORE_API auto HasRiskItems() const -> bool;
		ASSETCORE_API auto GetAffectedObjectCount() const -> uint64;
		ASSETCORE_API auto GetLegacyFieldCount() const -> uint64;
		ASSETCORE_API auto GetMigratedDataCount() const -> uint64;
		ASSETCORE_API auto GetRiskItemCount() const -> uint64;
	};

	// Adds a mutation to the active root load report. Calls outside package loading are ignored.
	ASSETCORE_API auto ReportAssetLoadMutation(
		DObject* Object,
		std::string HandlerId,
		std::string Summary,
		EAssetLoadMutationKind Kind = EAssetLoadMutationKind::NonUpgrade) -> void;

	// Resolves serialized object-reference payloads without exposing AssetCore's file implementation.
	class FAssetMigrationContext
	{
	public:
		ASSETCORE_API auto ReadObjectReference(const FAssetLegacyField& Field, DObject*& OutObject) const -> FAssetResult;
		ASSETCORE_API auto ReadObjectReferenceArray(
			const FAssetLegacyField& Field,
			std::vector<DObject*>& OutObjects) const -> FAssetResult;

	private:
		explicit FAssetMigrationContext(std::span<DObject* const> InObjects) : Objects(InObjects) {}
		std::span<DObject* const> Objects;

		friend class FAssetManager;
	};

	using FAssetStructureUpgrader = std::function<FAssetResult(
		DObject*,
		std::span<const FAssetLegacyField>,
		const FAssetMigrationContext&,
		std::vector<FAssetCompatibilityIssue>&)>;

	// Registers the engine-owned upgrader responsible for incompatible fields on a reflected class.
	ASSETCORE_API auto RegisterAssetStructureUpgrader(
		DClass* Class,
		std::string HandlerId,
		FAssetStructureUpgrader Upgrader) -> void;

	// Identifies the exact authored package bytes represented by an audit result.
	struct FAssetPackageFingerprint
	{
		uintmax_t FileSize = 0;
		int64 LastWriteTimeTicks = 0;
		FXxHash128 ContentHash;
		uint32 ReaderVersion = 0;

		auto operator==(const FAssetPackageFingerprint&) const -> bool = default;
	};

	// Requires an explicit opt-in before persistence may discard compatibility-risk payloads.
	struct FAssetPackageSaveOptions
	{
		bool bAllowCompatibilityDataLoss = false;
	};

	// Captures package ownership before a higher-level load request begins.
	struct FAssetPackageLoadSnapshot
	{
		std::vector<FAssetPath> LoadedPackages;
	};

	enum class EAssetRegistryEntryKind : uint8
	{
		Asset = 0,
		Redirector = 1
	};

	// Describes one discoverable package without loading its object graph.
	struct FAssetData
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uintmax_t FileSize = 0;

		// Native timestamp supports live comparisons; ticks provide stable snapshot persistence.
		std::filesystem::file_time_type LastWriteTime{};
		int64 LastWriteTimeTicks = 0;

		auto operator==(const FAssetData&) const -> bool = default;
	};

	enum class EAssetPathResolveState : uint8
	{
		Resolved,
		NotFound,
		MissingRedirectTarget,
		RedirectCycle,
		RedirectDepthExceeded,
		UnknownTargetClass,
		RedirectTypeMismatch,
		CorruptRedirector
	};

	struct FAssetPathResolveOptions
	{
		const DClass* ExpectedClass = nullptr;
	};

	struct FAssetPathResolveResult
	{
		EAssetPathResolveState State = EAssetPathResolveState::NotFound;
		FAssetPath RequestedPath;
		FAssetPath FinalPath;
		std::vector<FAssetPath> RedirectChain;
		std::optional<FAssetData> FinalAssetData;

		auto Succeeded() const -> bool { return State == EAssetPathResolveState::Resolved; }
		explicit operator bool() const { return Succeeded(); }
	};

	// Carries package metadata parsed without materializing package objects.
	struct FAssetPackageHeader
	{
		std::string AssetClassName;
		EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
		FAssetPath RedirectDestination;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uint64 ObjectCount = 0;

		// Number of source bytes consumed while parsing the header.
		uint64 BytesRead = 0;
	};

	// Reads and validates only the package metadata needed by discovery. BytesRead is exposed
	// so diagnostics and tests can verify that object payloads were not consumed.
	ASSETCORE_API auto ReadAssetPackageHeader(std::string_view PhysicalPath, FAssetPackageHeader& OutHeader) -> FAssetResult;
	ASSETCORE_API auto ValidateAssetPackageBytes(std::span<const uint8> Bytes) -> FAssetResult;
	// Produces redirect-free package bytes for runtime publication without changing authored files.
	ASSETCORE_API auto CanonicalizeAssetPackageForCook(
		std::span<const uint8> Bytes,
		std::vector<uint8>& OutBytes) -> FAssetResult;

	struct FAssetPackageSerializationOptions
	{
		// Returning false omits the field record entirely from this serialization.
		std::function<bool(const DObject*, const FProperty*)> PropertyFilter;
	};

	enum class EAssetBundleSavePhase : uint8
	{
		CreateDirectories,
		StagePackage,
		PublishPackage,
		PublishRootPackage,
		PublishRegistry
	};

	struct FAssetBundleSaveOptions
	{
		// The root package is published after every dependency package.
		DPackage* RootPackage = nullptr;

		// Explicit compatibility migration may retire preserved legacy fields
		// across one failure-atomic package bundle.
		bool bAllowCompatibilityDataLoss = false;

		// Tests and higher-level transactions may stop immediately before a phase.
		std::function<bool(EAssetBundleSavePhase, size_t)> ShouldFail;
	};

	// Serializes an asset package without publishing it or changing dirty/registry state.
	ASSETCORE_API auto SerializeAssetPackageBytes(
		DPackage* Package,
		std::vector<uint8>& OutBytes,
		const FAssetPackageSerializationOptions& Options = {}) -> FAssetResult;

	// Serializes and stages every package before making any package or registry
	// entry visible. Any publication failure restores prior files and leaves
	// package dirty state and registry contents unchanged.
	ASSETCORE_API auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options = {}) -> FAssetResult;

	// Removes a newly created, unpublished package from the loaded-object cache.
	// This is the rollback counterpart to CreateAsset and rejects visible assets.
	ASSETCORE_API auto DiscardUnpublishedPackage(DPackage* Package) -> FAssetResult;

	// Provides serialized main-asset fields without constructing objects or invoking PostLoad.
	enum class EAssetPackageObjectReferenceKind : uint8
	{
		Null,
		Internal,
		External
	};

	struct FAssetPackageObjectReference
	{
		EAssetPackageObjectReferenceKind Kind = EAssetPackageObjectReferenceKind::Null;
		uint64 ObjectId = 0;
		FAssetPath ExternalPath;
	};

	struct FAssetPackageField
	{
		std::string DeclaringClass;
		std::string Name;
		DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
		std::string TypeSignature;
		std::vector<uint8> Payload;
		uint32 SourceFormatVersion = 0;

		ASSETCORE_API auto TryReadString(std::string& OutValue) const -> bool;
		ASSETCORE_API auto TryReadStruct(DStruct* Struct, void* OutValue) const -> bool;
		ASSETCORE_API auto TryReadObjectReference(FAssetPackageObjectReference& OutValue) const -> bool;
		ASSETCORE_API auto TryReadObjectReferenceArray(
			std::vector<FAssetPackageObjectReference>& OutValues) const -> bool;

		template<typename T>
		auto TryReadScalar(T& OutValue) const -> bool
		{
			static_assert(std::is_trivially_copyable_v<T>);
			if (Payload.size() != sizeof(T)) return false;
			std::memcpy(&OutValue, Payload.data(), sizeof(T));
			return true;
		}
	};

	// Carries one serialized object and all of its fields without constructing it.
	struct FAssetPackageObjectInspection
	{
		uint64 Id = 0;
		uint64 OuterId = 0;
		std::string ClassName;
		std::string ObjectName;
		std::string ObjectPath;
		std::vector<FAssetPackageField> Fields;

		auto FindField(std::string_view Name) const -> const FAssetPackageField*
		{
			const auto It = std::ranges::find(Fields, Name, &FAssetPackageField::Name);
			return It == Fields.end() ? nullptr : &*It;
		}
	};

	// Carries every serialized object for lightweight tooling inspection.
	struct FAssetPackageInspection
	{
		FAssetPackageHeader Header;
		FAssetPackageFingerprint Fingerprint;
		std::vector<FAssetPackageObjectInspection> Objects;

		auto FindField(std::string_view Name) const -> const FAssetPackageField*
		{
			return Objects.empty() ? nullptr : Objects.front().FindField(Name);
		}

		auto FindObject(uint64 Id) const -> const FAssetPackageObjectInspection*
		{
			if (Id == 0 || Id > Objects.size()) return nullptr;
			const FAssetPackageObjectInspection& Object = Objects[static_cast<size_t>(Id - 1)];
			return Object.Id == Id ? &Object : nullptr;
		}
	};

	// Reads the complete serialized package into a field snapshot without resolving dependencies,
	// constructing objects, or invoking PostLoad.
	ASSETCORE_API auto InspectAssetPackage(std::string_view PhysicalPath, FAssetPackageInspection& OutInspection) -> FAssetResult;

	// Classifies one authored package edge in the unified reference graph.
	enum class EAssetReferenceKind : uint8
	{
		HardObject,
		SoftObject,
		Redirect
	};

	// Identifies one stable route hop from an authored field to a reference value.
	enum class EAssetReferenceRouteKind : uint8
	{
		FixedArray,
		ArrayElement,
		MapValue,
		StructField
	};

	struct FAssetReferenceRouteSegment
	{
		EAssetReferenceRouteKind Kind = EAssetReferenceRouteKind::FixedArray;
		uint64 Index = 0;
		// Map values use the canonical key token that orders authored Map entries.
		std::vector<uint8> MapKeyToken;
		std::string DeclaringType;
		std::string FieldName;

		auto operator==(const FAssetReferenceRouteSegment&) const -> bool = default;
	};

	// Describes one non-null authored occurrence extracted from authoritative DAST bytes.
	struct FAssetReferenceEdge
	{
		FAssetPath SourcePackage;
		FAssetPackageFingerprint SourceFingerprint;
		uint64 SourceObjectId = 0;
		std::string SourceClass;
		std::string DeclaringType;
		std::string FieldName;
		EAssetReferenceKind Kind = EAssetReferenceKind::HardObject;
		std::string ExpectedClass;
		FAssetPath TargetPath;
		std::vector<FAssetReferenceRouteSegment> Route;
		std::string DisplayRoute;

		auto operator==(const FAssetReferenceEdge&) const -> bool = default;
	};

	// Extracts direct and recursively nested hard/soft paths without constructing package objects,
	// invoking PostLoad, resolving targets, or changing package residency.
	ASSETCORE_API auto ExtractAssetReferences(
		const FAssetPath& SourcePackage,
		const FAssetPackageInspection& Inspection,
		std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult;

	// Selects whether registry discovery may reuse its persistent snapshot.
	enum class EAssetRegistryScanMode : uint8
	{
		Incremental,
		FullValidation
	};

	// Reports discovery work and I/O performed by the most recent registry scan.
	struct FAssetRegistryScanStats
	{
		uint64 Enumerated = 0;
		uint64 Reused = 0;
		uint64 Reparsed = 0;
		uint64 Removed = 0;
		uint64 Failed = 0;
		uint64 Redirectors = 0;
		uint64 HeaderReadAttempts = 0;
		uint64 HeaderBytesRead = 0;

		// Wall-clock scan duration in milliseconds.
		double DurationMilliseconds = 0.0;
	};

	struct FAssetReferenceIndexStats
	{
		uint64 ReusedSources = 0;
		uint64 ExtractedSources = 0;
		uint64 FailedSources = 0;
		uint64 PayloadReadAttempts = 0;
		uint64 PayloadBytesRead = 0;
	};

	// Owns the deterministic, fingerprint-bound derived graph for authored packages.
	class FAssetReferenceIndex
	{
	public:
		auto GetEdges() const -> std::span<const FAssetReferenceEdge> { return Edges; }
		ASSETCORE_API auto FindReferencers(const FAssetPath& Target) const
			-> std::vector<FAssetReferenceEdge>;
		ASSETCORE_API auto FindTargets(const FAssetPath& Source) const
			-> std::vector<FAssetPath>;
		auto IsComplete() const -> bool { return bComplete; }
		auto GetErrors() const -> std::span<const FAssetResult> { return Errors; }
		auto GetStats() const -> const FAssetReferenceIndexStats& { return Stats; }
		auto GetCacheWarning() const -> const std::string& { return CacheWarning; }

	private:
		std::vector<FAssetReferenceEdge> Edges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> SourceFingerprints;
		std::vector<FAssetResult> Errors;
		FAssetReferenceIndexStats Stats;
		std::string CacheWarning;
		bool bComplete = false;
		bool bSnapshotDirty = false;

		friend class FAssetRegistry;
		friend class FAssetManager;
	};

	// Identifies one persistent path occurrence outside a .dasset package.
	struct FAssetReferenceStoreOccurrence
	{
		std::string ProviderId;
		std::string StableId;
		FAssetPath TargetPath;
		std::string DisplayRoute;
		// Runtime roots join Cook reachability but retain their authored store value.
		std::string ExpectedClass;
		bool bCookRoot = false;

		auto operator==(const FAssetReferenceStoreOccurrence&) const -> bool = default;
	};

	struct FAssetReferenceRewrite
	{
		std::string StableId;
		FAssetPath SourcePath;
		FAssetPath DestinationPath;

		auto operator==(const FAssetReferenceRewrite&) const -> bool = default;
	};

	struct FAssetRedirectorFixupMapping
	{
		FAssetPath RedirectorPath;
		FAssetPath FinalPath;

		auto operator==(const FAssetRedirectorFixupMapping&) const -> bool = default;
	};

	// Captures one provider's deterministic occurrence set and durable-state fingerprint.
	struct FAssetReferenceStoreSnapshot
	{
		std::string ProviderId;
		uint64 ProviderVersion = 0;
		std::string Fingerprint;
		std::vector<FAssetReferenceStoreOccurrence> Occurrences;
	};

	// Supplies preconstructed package bytes owned semantically by an external
	// reference store so AssetCore can publish them through the shared journal.
	struct FAssetReferenceStorePackageRewrite
	{
		FAssetPath PackagePath;
		std::vector<uint8> PreBytes;
		std::vector<uint8> PostBytes;
	};

	// Owns a provider-prepared reversible write for one Fix Up transaction.
	struct FAssetReferenceStoreRewriteContribution
	{
		std::string Fingerprint;
		std::vector<FAssetReferenceRewrite> Rewrites;
		std::vector<FAssetReferenceStorePackageRewrite> PackageRewrites;
		std::function<FAssetResult()> Revalidate;
		std::function<FAssetResult()> Apply;
		std::function<FAssetResult()> Restore;
		std::function<FAssetResult()> Verify;
	};

	// Enumerates and transactionally rewrites persistent asset paths outside packages.
	class IAssetReferenceStore
	{
	public:
		virtual ~IAssetReferenceStore() = default;
		virtual auto CaptureSnapshot(FAssetReferenceStoreSnapshot& OutSnapshot)
			-> FAssetResult = 0;
		virtual auto PrepareRewrite(
			std::span<const FAssetReferenceRewrite> Rewrites,
			std::string_view ExpectedFingerprint,
			FAssetReferenceStoreRewriteContribution& OutContribution)
			-> FAssetResult = 0;
	};

	using FAssetReferenceStoreHandle = uint64;
	ASSETCORE_API auto RegisterAssetReferenceStore(IAssetReferenceStore* Store)
		-> FAssetReferenceStoreHandle;
	ASSETCORE_API auto UnregisterAssetReferenceStore(
		FAssetReferenceStoreHandle Handle) -> void;

	enum class EAssetRedirectorFixupMode : uint8
	{
		RewriteOnly,
		RewriteAndDelete
	};

	// Owns an immutable, fingerprint-bound Fix Up plan and its retained journal.
	class FAssetRedirectorFixupPlan
	{
	public:
		ASSETCORE_API auto GetMode() const -> EAssetRedirectorFixupMode;
		ASSETCORE_API auto GetRegistryRevision() const -> uint64;
		ASSETCORE_API auto GetRedirectors() const -> std::span<const FAssetPath>;
		ASSETCORE_API auto GetFinalPathMappings() const
			-> std::span<const FAssetRedirectorFixupMapping>;
		ASSETCORE_API auto GetPackageOccurrences() const
			-> std::span<const FAssetReferenceEdge>;
		ASSETCORE_API auto GetStoreOccurrences() const
			-> std::span<const FAssetReferenceStoreOccurrence>;
		ASSETCORE_API auto GetDeletableRedirectors() const
			-> std::span<const FAssetPath>;

	private:
		struct FState;
		std::shared_ptr<FState> State;

		friend class FAssetManager;
	};

	enum class EAssetRedirectorFixupFailurePoint : uint8
	{
		None,
		PreparePackage,
		PrepareStore,
		StageOriginal,
		PublishPackage,
		ApplyStore,
		Verify,
		DeleteRedirector,
		PublishRegistry,
		CompensatePackage,
		CompensateStore
	};

	ASSETCORE_API auto SetAssetRedirectorFixupFailurePointForTesting(
		EAssetRedirectorFixupFailurePoint Point,
		uint32 Occurrence = 1) -> void;

	// Describes one requested real-asset relocation in an atomic batch.
	struct FAssetRelocationMapping
	{
		FAssetPath SourcePath;
		FAssetPath DestinationPath;

		auto operator==(const FAssetRelocationMapping&) const -> bool = default;
	};

	// Restricts a class contribution to files and reversible live state owned
	// exclusively by the moving asset.
	struct FAssetOwnedPayloadRelocation
	{
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Files;
		std::function<void()> Apply;
		std::function<void()> Restore;
	};

	using FAssetOwnedPayloadRelocator = std::function<FAssetResult(
		DObject*,
		const FAssetPath&,
		const FAssetPath&,
		FAssetOwnedPayloadRelocation&)>;
	ASSETCORE_API auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator) -> void;

	// Receives committed relocation direction changes for transient editor and
	// cache state. Observers cannot reject or roll back authored publication.
	class IAssetMoveObserver
	{
	public:
		virtual ~IAssetMoveObserver() = default;
		virtual auto OnAssetsRelocated(
			std::span<const FAssetRelocationMapping> Mappings) -> void = 0;
	};

	using FAssetMoveObserverHandle = uint64;
	ASSETCORE_API auto RegisterAssetMoveObserver(IAssetMoveObserver* Observer)
		-> FAssetMoveObserverHandle;
	ASSETCORE_API auto UnregisterAssetMoveObserver(
		FAssetMoveObserverHandle Handle) -> void;

	// Owns the immutable relocation plan and its retained pre/post publication
	// journal. AssetCore alone advances the internal transaction state.
	class FAssetRelocationBatchToken
	{
	public:
		ASSETCORE_API auto GetRegistryRevision() const -> uint64;
		ASSETCORE_API auto GetMappings() const
			-> std::span<const FAssetRelocationMapping>;

	private:
		struct FState;
		std::shared_ptr<FState> State;

		friend class FAssetManager;
	};

	// Failure seams are deterministic and one-shot so transaction-boundary tests
	// can prove compensation without changing production publication behavior.
	enum class EAssetRelocationFailurePoint : uint8
	{
		None,
		PrepareOutput,
		StageOriginal,
		PublishRealAsset,
		PublishOwnedPayload,
		PublishRedirector,
		UpdateLoadedPackage,
		PublishRegistry,
		CompensateFile,
		CompensateLoadedPackage,
	};

	ASSETCORE_API auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence = 1) -> void;

	// Describes companion files contributed to an asset deletion.
	struct FAssetDeleteContribution
	{
		std::vector<std::filesystem::path> Files;
	};

	// Captures the references and transient state that determine whether deletion is safe.
	struct FAssetDeleteAnalysis
	{
		FAssetPath AssetPath;
		std::vector<FAssetPath> DirectReferencers;
		std::vector<std::filesystem::path> CompanionFiles;
		FAssetPath RedirectDestination;

		// Non-fatal inspection failure; deletion remains available for the main package file.
		std::string Warning;
		bool bLoaded = false;
		bool bLoading = false;
		bool bRedirector = false;

		// A loaded package is cache state, not a usage claim. Deletion safely unloads it after
		// persistent referencers have been ruled out.
		auto CanDelete() const -> bool { return DirectReferencers.empty() && !bLoading; }
	};

	// Classifies a condition that prevents an asset set from being deleted as one batch.
	enum class EAssetDeletionBatchBlocker : uint8
	{
		MissingAsset,
		ExternalPersistentReference,
		ExternalLoadedReference,
		RedirectorTargetNotSelected,
		TargetRedirectorsNotSelected,
		LoadingPackage,
		DirtyPackage,
		ReferenceStoreInspectionFailed,
		CompanionInspectionFailed,
		CompanionOwnershipConflict,
		ExternalCompanionOwner,
	};

	// Identifies one actionable batch-deletion failure without flattening it into UI copy.
	struct FAssetDeletionBatchBlocker
	{
		EAssetDeletionBatchBlocker Kind = EAssetDeletionBatchBlocker::MissingAsset;
		FAssetPath AssetPath;
		FAssetPath RelatedAssetPath;
		std::filesystem::path PhysicalPath;
		std::string Details;
	};

	// Calls out a destructive-but-valid target-plus-alias deletion before confirmation.
	struct FAssetDeletionBatchWarning
	{
		FAssetPath TargetPath;
		std::vector<FAssetPath> RedirectorPaths;
		std::vector<FAssetPath> SoftReferencerPaths;
		std::vector<std::string> ExternalOccurrences;
		std::string Details;

		auto operator==(const FAssetDeletionBatchWarning&) const -> bool = default;
	};

	// Captures one asset's registry and companion state for reversible batch deletion.
	struct FAssetDeletionBatchEntry
	{
		FAssetData RegistryEntry;
		std::vector<std::filesystem::path> CompanionFiles;
		bool bLoaded = false;
	};

	// Owns the immutable-order AssetCore portion of a content-deletion plan. AssetCore
	// changes package cache and registry projection through this token; its caller owns
	// all physical file staging.
	class FAssetDeletionBatchToken
	{
	public:
		auto GetRegistryRevision() const -> uint64 { return RegistryRevision; }
		auto GetEntries() const -> std::span<const FAssetDeletionBatchEntry>
		{
			return Entries;
		}
		auto GetWarnings() const -> std::span<const FAssetDeletionBatchWarning>
		{
			return Warnings;
		}

	private:
		uint64 RegistryRevision = 0;
		uint64 ReferenceStoreRevision = 0;
		std::vector<FAssetDeletionBatchEntry> Entries;
		std::vector<FAssetDeletionBatchWarning> Warnings;
		std::vector<std::filesystem::path> PhysicalRoots;

		friend class FAssetManager;
	};

	using FAssetDeleteContributor = std::function<FAssetResult(const FAssetData&, const FAssetPackageInspection&, FAssetDeleteContribution&)>;
	ASSETCORE_API auto RegisterAssetDeleteContributor(DClass* Class, FAssetDeleteContributor Contributor) -> void;

	enum class EAssetCompanionOwnershipState : uint8
	{
		Unclaimed,
		Owned,
		Ambiguous,
	};

	struct FAssetCompanionOwnership
	{
		EAssetCompanionOwnershipState State =
			EAssetCompanionOwnershipState::Unclaimed;
		std::vector<FAssetPath> Owners;
	};

	// Queries the exact files reported by registered deletion contributors without
	// loading packages or inferring ownership from filenames.
	ASSETCORE_API auto QueryAssetCompanionOwnership(
		const std::filesystem::path& PhysicalPath,
		FAssetCompanionOwnership& OutOwnership) -> FAssetResult;

	// Owns the discovered asset index and its persistent snapshot state.
	class FAssetRegistry
	{
	public:
		ASSETCORE_API auto ScanMountedContent(EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental) -> FAssetResult;
		ASSETCORE_API auto FlushPersistentSnapshot() -> void;
		ASSETCORE_API auto FindAssetExact(const FAssetPath& Path) const -> const FAssetData*;
		ASSETCORE_API auto ResolveAssetPath(
			const FAssetPath& Path,
			const FAssetPathResolveOptions& Options = {}) const -> FAssetPathResolveResult;
		ASSETCORE_API auto FindRedirectorsTo(const FAssetPath& Destination) const
			-> std::vector<FAssetPath>;
		auto GetAssets() const -> const std::unordered_map<FAssetPath, FAssetData>& { return Assets; }
		auto GetScanErrors() const -> const std::vector<FAssetResult>& { return ScanErrors; }
		auto GetLastScanStats() const -> const FAssetRegistryScanStats& { return LastScanStats; }
		auto GetCacheWarning() const -> const std::string& { return CacheWarning; }
		auto IsPersistentSnapshotDirty() const -> bool { return bPersistentSnapshotDirty; }
		auto GetRevision() const -> uint64 { return Revision; }
		auto GetReferenceIndex() const -> const FAssetReferenceIndex& { return ReferenceIndex; }
		// Builds a final-real-path Cook closure from explicit and registered runtime
		// roots plus hard/soft dependencies. It never loads or mutates authored state.
		ASSETCORE_API auto BuildCookReachability(
			std::span<const FAssetPath> Roots,
			std::vector<FAssetPath>& OutPackages) const -> FAssetResult;

	private:
		auto AddOrUpdate(FAssetData Data) -> void;
		auto Remove(const FAssetPath& Path) -> void;
		auto RefreshReferencesForAsset(const FAssetData& Data) -> bool;
		auto RemoveReferencesFromSource(const FAssetPath& Path) -> bool;
		auto RebuildRedirectorIndex() -> void;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::unordered_map<FAssetPath, std::vector<FAssetPath>> RedirectorsByDestination;
		std::vector<FAssetResult> ScanErrors;
		FAssetRegistryScanStats LastScanStats;
		std::string CacheWarning;
		bool bPersistentSnapshotDirty = false;
		FAssetReferenceIndex ReferenceIndex;

		// Monotonically changes when the visible registry contents change.
		uint64 Revision = 1;

		friend class FAssetManager;
		friend ASSETCORE_API auto SavePackagesAtomically(
			std::span<DPackage* const>,
			const FAssetBundleSaveOptions&) -> FAssetResult;
	};

	// Coordinates package persistence, loading, and registry consistency.
	class FAssetManager
	{
	public:
		ASSETCORE_API static auto Get() -> FAssetManager&;

		ASSETCORE_API auto CreateAsset(const FAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult;
		ASSETCORE_API auto CreateRedirector(
			const FAssetPath& RedirectorPath,
			const FAssetPath& DestinationPath,
			DAssetRedirector*& OutRedirector) -> FAssetResult;
		ASSETCORE_API auto LoadAsset(
			const FAssetPath& Path,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		ASSETCORE_API auto LoadAsset(
			const FAssetPath& Path,
			const DClass* ExpectedClass,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		ASSETCORE_API auto SavePackage(
			DPackage* Package,
			const FAssetPackageSaveOptions& Options = {}) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetRelocationBatch(
			std::span<const FAssetRelocationMapping> Mappings,
			FAssetRelocationBatchToken& OutToken) -> FAssetResult;
		ASSETCORE_API auto RevalidateAssetRelocationBatch(
			const FAssetRelocationBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto ApplyAssetRelocationBatch(
			const FAssetRelocationBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto RestoreAssetRelocationBatch(
			const FAssetRelocationBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto AnalyzeRedirectorFixup(
			std::span<const FAssetPath> Redirectors,
			EAssetRedirectorFixupMode Mode,
			FAssetRedirectorFixupPlan& OutPlan) -> FAssetResult;
		ASSETCORE_API auto RevalidateRedirectorFixup(
			const FAssetRedirectorFixupPlan& Plan) -> FAssetResult;
		ASSETCORE_API auto ApplyRedirectorFixup(
			const FAssetRedirectorFixupPlan& Plan) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetDeletionBatch(
			std::span<const FAssetPath> Paths,
			std::span<const std::filesystem::path> PhysicalRoots,
			FAssetDeletionBatchToken& OutToken,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		ASSETCORE_API auto RevalidateAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token,
			std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
		ASSETCORE_API auto UnloadAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto ApplyAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		// Commit-only half used after the editor transaction has already revalidated,
		// unloaded, and staged the exact token-owned physical roots.
		ASSETCORE_API auto RemoveAssetDeletionBatchRegistryProjection(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto RestoreAssetDeletionBatch(
			const FAssetDeletionBatchToken& Token) -> FAssetResult;
		ASSETCORE_API auto DeleteAsset(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto FindLoadedPackage(const FAssetPath& Path) const -> DPackage*;
		ASSETCORE_API auto UnloadPackage(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot;
		ASSETCORE_API auto ReleasePackagesLoadedSince(
			const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult;
		// Commit-only migration seam. Callers must fully validate every entry before
		// publishing the complete selected bundle through this no-fail projection step.
		ASSETCORE_API auto PublishMigratedPackageRegistryEntries(
			std::span<FAssetData> Entries) -> void;
		// Reopens an empty manager after Shutdown().
		ASSETCORE_API auto Initialize() -> void;
		ASSETCORE_API auto StopAcceptingRequests() -> void;
		auto IsAcceptingRequests() const -> bool { return bAcceptingRequests; }
		ASSETCORE_API auto Shutdown() -> void;
		// Configuration is rejected after the first successful package load until shutdown.
		ASSETCORE_API auto ConfigurePackageLoadContext(FPackageLoadContext InContext) -> FAssetResult;
		auto GetPackageLoadContext() const -> const FPackageLoadContext& { return PackageLoadContext; }

		auto GetRegistry() -> FAssetRegistry& { return Registry; }
		auto GetRegistry() const -> const FAssetRegistry& { return Registry; }

	private:
		FAssetManager();
		auto LoadAssetExact(
			const FAssetPath& Path,
			const DClass* ExpectedClass,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto LoadPackageInternal(
			const FAssetPath& Path,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto ResolveSoftObjectInternal(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult;
		auto LoadSoftObjectInternal(
			FSoftObjectPtr& Reference,
			const DClass* ExpectedClass,
			DObject*& OutObject,
			ESoftObjectNullPolicy NullPolicy,
			FAssetLoadReport* OutReport) -> FAssetResult;
		auto IsPackageReferenced(const DPackage* Package) const -> bool;

		FAssetRegistry Registry;

		// Loaded packages are owned by the object system and indexed here for reuse.
		std::unordered_map<FAssetPath, DPackage*> LoadedPackages;

		// Tracks active loads to reject dependency cycles.
		std::unordered_set<FAssetPath> LoadingPackages;

		// Packages with unhandled compatibility data cannot be persisted without explicit consent.
		std::unordered_set<DPackage*> CompatibilityRiskPackages;

		// Outermost loads commit residency and registry projection as one boundary.
		uint32 LoadDepth = 0;
		std::vector<FAssetPath> TransactionPackages;
		std::vector<FAssetData> TransactionRegistryEntries;
		FPackageLoadContext PackageLoadContext;
		bool bPackageLoadStarted = false;
		bool bAcceptingRequests = true;

		friend ASSETCORE_API auto SavePackagesAtomically(
			std::span<DPackage* const>,
			const FAssetBundleSaveOptions&) -> FAssetResult;
		friend ASSETCORE_API auto LoadPackageForMigration(
			const FAssetPath&,
			DPackage*&,
			FAssetLoadReport*) -> FAssetResult;
		friend ASSETCORE_API auto ResolveSoftObject(
			FSoftObjectPtr&,
			const DClass*,
			ESoftObjectNullPolicy) -> FSoftObjectResolveResult;
		friend ASSETCORE_API auto LoadSoftObject(
			FSoftObjectPtr&,
			const DClass*,
			DObject*&,
			ESoftObjectNullPolicy,
			FAssetLoadReport*) -> FAssetResult;
	};

	template<typename T>
	auto CreateAsset(const FAssetPath& Path, T*& OutAsset) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = FAssetManager::Get().CreateAsset(Path, T::StaticClass(), sizeof(T), Object);
		OutAsset = Result ? Cast<T>(Object) : nullptr;
		return Result;
	}

	template<typename T>
	auto LoadAsset(const FAssetPath& Path, T*& OutAsset, FAssetLoadReport* OutReport = nullptr) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = FAssetManager::Get().LoadAsset(
			Path, T::StaticClass(), Object, OutReport);
		OutAsset = static_cast<T*>(Object);
		return Result;
	}

	ASSETCORE_API auto ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject) -> FSoftObjectResolveResult;

	ASSETCORE_API auto LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;

	template<typename T>
	auto ResolveSoftObject(
		TSoftObjectPtr<T>& Reference,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject) -> TSoftObjectResolveResult<T>
	{
		static_assert(std::is_base_of_v<DObject, T>);
		FSoftObjectResolveResult Result = ResolveSoftObject(
			Reference.GetBase(), T::StaticClass(), NullPolicy);
		return {
			.Result = std::move(Result.Result),
			.State = Result.State,
			.Object = static_cast<T*>(Result.Object),
			.ResolvedPath = std::move(Result.ResolvedPath),
			.bRedirected = Result.bRedirected};
	}

	template<typename T>
	auto LoadSoftObject(
		TSoftObjectPtr<T>& Reference,
		T*& OutObject,
		ESoftObjectNullPolicy NullPolicy = ESoftObjectNullPolicy::Reject,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = LoadSoftObject(
			Reference.GetBase(), T::StaticClass(), Object, NullPolicy, OutReport);
		OutObject = Result ? static_cast<T*>(Object) : nullptr;
		return Result;
	}

	ASSETCORE_API auto LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
	// Loads an exact package without redirect resolution for explicit migration.
	ASSETCORE_API auto LoadPackageForMigration(
		const FAssetPath& Path,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
	// True only while the migration loader constructs authored objects. Reflected
	// deserialization and registered structure upgrades still run, but resource
	// classes must not build or publish runtime-only state in this headless context.
	ASSETCORE_API auto IsAssetMigrationLoad() -> bool;
	ASSETCORE_API auto CreateAssetRedirector(
		const FAssetPath& RedirectorPath,
		const FAssetPath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult;
	ASSETCORE_API auto SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions& Options = {}) -> FAssetResult;
	ASSETCORE_API auto AnalyzeAssetRelocationBatch(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetRelocationBatchToken& OutToken) -> FAssetResult;
	ASSETCORE_API auto RevalidateAssetRelocationBatch(
		const FAssetRelocationBatchToken& Token) -> FAssetResult;
	ASSETCORE_API auto ApplyAssetRelocationBatch(
		const FAssetRelocationBatchToken& Token) -> FAssetResult;
	ASSETCORE_API auto RestoreAssetRelocationBatch(
		const FAssetRelocationBatchToken& Token) -> FAssetResult;
	ASSETCORE_API auto AnalyzeRedirectorFixup(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupPlan& OutPlan) -> FAssetResult;
	ASSETCORE_API auto RevalidateRedirectorFixup(
		const FAssetRedirectorFixupPlan& Plan) -> FAssetResult;
	ASSETCORE_API auto ApplyRedirectorFixup(
		const FAssetRedirectorFixupPlan& Plan) -> FAssetResult;
	ASSETCORE_API auto FixUpRedirectors(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode = EAssetRedirectorFixupMode::RewriteAndDelete)
		-> FAssetResult;
	ASSETCORE_API auto FixUpAllRedirectors(
		EAssetRedirectorFixupMode Mode = EAssetRedirectorFixupMode::RewriteAndDelete)
		-> FAssetResult;
	ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
	ASSETCORE_API auto AnalyzeAssetDeletionBatch(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionBatchToken& OutToken,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
	ASSETCORE_API auto RevalidateAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult;
	ASSETCORE_API auto UnloadAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult;
	ASSETCORE_API auto ApplyAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult;
	ASSETCORE_API auto RemoveAssetDeletionBatchRegistryProjection(
		const FAssetDeletionBatchToken& Token) -> FAssetResult;
	ASSETCORE_API auto RestoreAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult;
	ASSETCORE_API auto DeleteAsset(const FAssetPath& Path) -> FAssetResult;
	ASSETCORE_API auto FindLoadedPackage(const FAssetPath& Path) -> DPackage*;
	ASSETCORE_API auto UnloadPackage(const FAssetPath& Path) -> FAssetResult;
	ASSETCORE_API auto CapturePackageLoadSnapshot() -> FAssetPackageLoadSnapshot;
	ASSETCORE_API auto ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult;
	ASSETCORE_API auto ShutdownAssetManager() -> void;
	ASSETCORE_API auto ConfigurePackageLoadContext(FPackageLoadContext Context) -> FAssetResult;
	ASSETCORE_API auto GetPackageLoadContext() -> const FPackageLoadContext&;
	ASSETCORE_API auto GetAssetRegistry() -> FAssetRegistry&;
}
