#pragma once

#include "AssetCoreAPI.h"
#include "CookedAsset.h"
#include "DObject/CoreDObject.h"
#include "Hash/XxHash.h"

namespace Durin::Asset
{
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
		ReadOnlyMode
	};

	// Returns an asset operation status with an optional diagnostic message.
	struct FAssetResult
	{
		EAssetError Error = EAssetError::None;
		std::string Message;

		auto Succeeded() const -> bool { return Error == EAssetError::None; }
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

		auto operator==(const FAssetPackageFingerprint&) const -> bool = default;
	};

	// Requires an explicit opt-in before persistence may discard compatibility-risk payloads.
	struct FAssetPackageSaveOptions
	{
		bool bAllowCompatibilityDataLoss = false;
		std::optional<FAssetPackageFingerprint> ExpectedFingerprint;
	};

	// Describes one discoverable package without loading its object graph.
	struct FAssetData
	{
		FAssetPath PackagePath;
		std::string PhysicalPath;
		std::string AssetClassName;
		uint32 FormatVersion = 0;
		std::vector<FAssetPath> Dependencies;
		uintmax_t FileSize = 0;

		// Native timestamp supports live comparisons; ticks provide stable snapshot persistence.
		std::filesystem::file_time_type LastWriteTime{};
		int64 LastWriteTimeTicks = 0;

		auto operator==(const FAssetData&) const -> bool = default;
	};

	// Carries package metadata parsed without materializing package objects.
	struct FAssetPackageHeader
	{
		std::string AssetClassName;
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

	using FAssetStructureInspectionUpgrader = std::function<FAssetResult(
		const FAssetPackageInspection&,
		const FAssetPackageObjectInspection&,
		std::span<const FAssetLegacyField>,
		std::vector<FAssetCompatibilityIssue>&)>;

	// Registers an object-free counterpart to a structure upgrader for project-wide auditing.
	ASSETCORE_API auto RegisterAssetStructureInspectionUpgrader(
		std::string QualifiedClassName,
		std::string HandlerId,
		FAssetStructureInspectionUpgrader Upgrader) -> void;

	enum class EAssetPackageAuditState : uint8
	{
		NotAudited,
		UpToDate,
		SafeUpgrade,
		RiskyUpgrade,
		RewriteAvailable,
		BlockedUnsupported,
		BlockedReadOnly,
		BlockedLoadMutation,
		AuditFailed,
		Stale
	};

	// Describes the object-free upgrade state of one registry package.
	struct FAssetPackageAuditReport
	{
		FAssetPath PackagePath;
		std::string AssetClassName;
		uint32 FormatVersion = 0;
		FAssetPackageFingerprint Fingerprint;
		EAssetPackageAuditState State = EAssetPackageAuditState::NotAudited;
		std::vector<FAssetCompatibilityIssue> CompatibilityIssues;
		std::string Diagnostic;

		auto HasRiskItems() const -> bool
		{
			return std::ranges::any_of(CompatibilityIssues, [](const FAssetCompatibilityIssue& Issue) {
				return Issue.Risk != EAssetCompatibilityRisk::None;
			});
		}
	};

	ASSETCORE_API auto AuditAssetPackage(
		const FAssetData& Data,
		FAssetPackageAuditReport& OutReport) -> FAssetResult;

	struct FAssetPackageUpgradeResult
	{
		FAssetPath PackagePath;
		EAssetPackageAuditState State = EAssetPackageAuditState::NotAudited;
		FAssetLoadReport LoadReport;
		bool bSaved = false;
		std::string Diagnostic;
	};

	struct FAssetPackageUpgradeOptions
	{
		bool bAllowCompatibilityDataLoss = false;
	};

	struct FAssetUpgradeSessionProgress
	{
		uint64 Total = 0;
		uint64 Completed = 0;
		uint64 UpToDate = 0;
		uint64 Safe = 0;
		uint64 Risky = 0;
		uint64 Blocked = 0;
		uint64 Failed = 0;
		uint64 Stale = 0;
	};

	// Owns one immutable-order audit snapshot for editor and automation consumers.
	struct FAssetUpgradeSessionReport
	{
		uint64 RegistryRevision = 0;
		std::vector<FAssetPackageAuditReport> Packages;
		FAssetUpgradeSessionProgress Progress;

		ASSETCORE_API auto RebuildProgressAndSort() -> void;
		ASSETCORE_API auto FindPackage(const FAssetPath& Path) const
			-> const FAssetPackageAuditReport*;
	};

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
		uint64 HeaderReadAttempts = 0;
		uint64 HeaderBytesRead = 0;

		// Wall-clock scan duration in milliseconds.
		double DurationMilliseconds = 0.0;
	};

	// Describes files and reversible state changes contributed to an asset move.
	struct FAssetMoveContribution
	{
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Files;
		std::function<void()> Apply;
		std::function<void()> Rollback;
	};

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

		// Non-fatal inspection failure; deletion remains available for the main package file.
		std::string Warning;
		bool bLoaded = false;
		bool bLoading = false;

		// A loaded package is cache state, not a usage claim. Deletion safely unloads it after
		// persistent referencers have been ruled out.
		auto CanDelete() const -> bool { return DirectReferencers.empty() && !bLoading; }
	};

	using FAssetMoveContributor = std::function<FAssetResult(DObject*, const FAssetPath&, const FAssetPath&, FAssetMoveContribution&)>;
	ASSETCORE_API auto RegisterAssetMoveContributor(DClass* Class, FAssetMoveContributor Contributor) -> void;
	using FAssetDeleteContributor = std::function<FAssetResult(const FAssetData&, const FAssetPackageInspection&, FAssetDeleteContribution&)>;
	ASSETCORE_API auto RegisterAssetDeleteContributor(DClass* Class, FAssetDeleteContributor Contributor) -> void;

	// Owns the discovered asset index and its persistent snapshot state.
	class FAssetRegistry
	{
	public:
		ASSETCORE_API auto ScanMountedContent(EAssetRegistryScanMode Mode = EAssetRegistryScanMode::Incremental) -> FAssetResult;
		ASSETCORE_API auto FlushPersistentSnapshot() -> void;
		ASSETCORE_API auto FindAsset(const FAssetPath& Path) const -> const FAssetData*;
		auto GetAssets() const -> const std::unordered_map<FAssetPath, FAssetData>& { return Assets; }
		auto GetScanErrors() const -> const std::vector<FAssetResult>& { return ScanErrors; }
		auto GetLastScanStats() const -> const FAssetRegistryScanStats& { return LastScanStats; }
		auto GetCacheWarning() const -> const std::string& { return CacheWarning; }
		auto IsPersistentSnapshotDirty() const -> bool { return bPersistentSnapshotDirty; }
		auto GetRevision() const -> uint64 { return Revision; }

	private:
		auto AddOrUpdate(FAssetData Data) -> void;
		auto Remove(const FAssetPath& Path) -> void;
		std::unordered_map<FAssetPath, FAssetData> Assets;
		std::vector<FAssetResult> ScanErrors;
		FAssetRegistryScanStats LastScanStats;
		std::string CacheWarning;
		bool bPersistentSnapshotDirty = false;

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
		ASSETCORE_API auto LoadAsset(
			const FAssetPath& Path,
			DObject*& OutAsset,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		ASSETCORE_API auto SavePackage(
			DPackage* Package,
			const FAssetPackageSaveOptions& Options = {}) -> FAssetResult;
		ASSETCORE_API auto ExecutePackageUpgrade(
			const FAssetPackageAuditReport& Audit,
			FAssetPackageUpgradeResult& OutResult,
			const FAssetPackageUpgradeOptions& Options = {}) -> FAssetResult;
		ASSETCORE_API auto MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult;
		ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
		ASSETCORE_API auto DeleteAsset(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto FindLoadedPackage(const FAssetPath& Path) const -> DPackage*;
		ASSETCORE_API auto UnloadPackage(const FAssetPath& Path) -> FAssetResult;
		ASSETCORE_API auto Shutdown() -> void;
		// Configuration is rejected after the first successful package load until shutdown.
		ASSETCORE_API auto ConfigurePackageLoadContext(FPackageLoadContext InContext) -> FAssetResult;
		auto GetPackageLoadContext() const -> const FPackageLoadContext& { return PackageLoadContext; }

		auto GetRegistry() -> FAssetRegistry& { return Registry; }
		auto GetRegistry() const -> const FAssetRegistry& { return Registry; }

	private:
		FAssetManager() = default;
		auto LoadPackageInternal(
			const FAssetPath& Path,
			DPackage*& OutPackage,
			FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
		auto IsPackageReferenced(const DPackage* Package) const -> bool;

		FAssetRegistry Registry;

		// Loaded packages are owned by the object system and indexed here for reuse.
		std::unordered_map<FAssetPath, DPackage*> LoadedPackages;

		// Tracks active loads to reject dependency cycles.
		std::unordered_set<FAssetPath> LoadingPackages;

		// Packages with unhandled compatibility data cannot be persisted without explicit consent.
		std::unordered_set<DPackage*> CompatibilityRiskPackages;

		// Outermost loads commit TransactionPackages as one rollback boundary.
		uint32 LoadDepth = 0;
		std::vector<FAssetPath> TransactionPackages;
		FPackageLoadContext PackageLoadContext;
		bool bPackageLoadStarted = false;

		friend ASSETCORE_API auto SavePackagesAtomically(
			std::span<DPackage* const>,
			const FAssetBundleSaveOptions&) -> FAssetResult;
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
		FAssetResult Result = FAssetManager::Get().LoadAsset(Path, Object, OutReport);
		if (Result && Object && !Object->IsA<T>())
		{
			OutAsset = nullptr;
			return {EAssetError::TypeMismatch, std::format("Asset {} is not a {}.", Path.ToString(), T::StaticClass()->GetQualifiedName().ToString())};
		}
		OutAsset = static_cast<T*>(Object);
		return Result;
	}

	ASSETCORE_API auto LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport = nullptr) -> FAssetResult;
	ASSETCORE_API auto SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions& Options = {}) -> FAssetResult;
	ASSETCORE_API auto ExecutePackageUpgrade(
		const FAssetPackageAuditReport& Audit,
		FAssetPackageUpgradeResult& OutResult,
		const FAssetPackageUpgradeOptions& Options = {}) -> FAssetResult;
	ASSETCORE_API auto MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult;
	ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
	ASSETCORE_API auto DeleteAsset(const FAssetPath& Path) -> FAssetResult;
	ASSETCORE_API auto FindLoadedPackage(const FAssetPath& Path) -> DPackage*;
	ASSETCORE_API auto UnloadPackage(const FAssetPath& Path) -> FAssetResult;
	ASSETCORE_API auto ShutdownAssetManager() -> void;
	ASSETCORE_API auto ConfigurePackageLoadContext(FPackageLoadContext Context) -> FAssetResult;
	ASSETCORE_API auto GetPackageLoadContext() -> const FPackageLoadContext&;
	ASSETCORE_API auto GetAssetRegistry() -> FAssetRegistry&;
}
