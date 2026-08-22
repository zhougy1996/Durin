#pragma once

#include "Asset/Load.h"
#include "Asset/PackageAuthoring.h"
#include "Asset/References.h"

namespace Durin::Asset
{
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
		std::vector<std::byte> PreBytes;
		std::vector<std::byte> PostBytes;
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
			FAssetReferenceStoreRewriteContribution& OutContribution
		)
			-> FAssetResult = 0;
	};

	using FAssetReferenceStoreHandle = uint64;
	ASSETCORE_API auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store,
		FModuleOwnedCallbackGate OwnerGate = {}
	)
		-> FAssetReferenceStoreHandle;
	ASSETCORE_API auto UnregisterAssetReferenceStore(
		FAssetReferenceStoreHandle Handle
	) -> void;

	enum class EAssetRedirectorFixupMode : uint8
	{
		RewriteOnly,
		RewriteAndDelete
	};

	// Immutable preview of one fingerprint-bound Fix Up transaction.
	class FAssetRedirectorFixupSummary
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
		EAssetRedirectorFixupMode Mode =
			EAssetRedirectorFixupMode::RewriteOnly;
		uint64 RegistryRevision = 0;
		std::vector<FAssetPath> Redirectors;
		std::vector<FAssetRedirectorFixupMapping> FinalPathMappings;
		std::vector<FAssetReferenceEdge> PackageOccurrences;
		std::vector<FAssetReferenceStoreOccurrence> StoreOccurrences;
		std::vector<FAssetPath> DeletableRedirectors;

#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};

	// Describes one requested real-asset relocation in an atomic batch.
	struct FAssetRelocationMapping
	{
		FAssetPath SourcePath;
		FAssetPath DestinationPath;

		auto operator==(const FAssetRelocationMapping&) const -> bool = default;
	};

	enum class EAssetMutationOperationKind : uint8
	{
		Relocation,
		RedirectorFixup,
		Deletion,
	};

	enum class EAssetMutationTransactionState : uint8
	{
		Empty,
		Prepared,
		Committed,
		Undone,
		RecoveryRequired,
	};

	// Immutable description of the durable state covered by one transaction.
	class FAssetMutationSummary
	{
	public:
		FAssetMutationSummary() = default;
		FAssetMutationSummary(
			EAssetMutationOperationKind InOperationKind,
			uint64 InRegistryRevision,
			std::vector<FAssetPath> InScope
		)
			: OperationKind(InOperationKind)
			, RegistryRevision(InRegistryRevision)
			, Scope(std::move(InScope))
		{
		}

		auto GetOperationKind() const -> EAssetMutationOperationKind
		{
			return OperationKind;
		}
		auto GetRegistryRevision() const -> uint64 { return RegistryRevision; }
		auto GetScope() const -> std::span<const FAssetPath> { return Scope; }

	private:
		EAssetMutationOperationKind OperationKind =
			EAssetMutationOperationKind::Relocation;
		uint64 RegistryRevision = 0;
		std::vector<FAssetPath> Scope;
	};

	struct FAssetMutationResultDetails
	{
		FAssetResult Result;
		EAssetMutationTransactionState State =
			EAssetMutationTransactionState::Empty;
		uint64 RegistryRevision = 0;
		bool bStateRestored = false;
		bool bRecoveryRequired = false;
		std::vector<FAssetPath> RewrittenPaths;
		std::vector<FAssetPath> RetainedPaths;
		std::vector<FAssetPath> DeletedPaths;
		std::vector<FAssetPath> SkippedPaths;
		std::vector<FAssetPath> FailedPaths;
	};

	// Cross-module extension point for asset classes that exclusively own
	// sidecar files or reversible live state. AssetCore intentionally provides
	// no built-in registration: the module defining such an asset class owns its
	// relocation policy without creating a reverse module dependency.
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
		FAssetOwnedPayloadRelocation&
	)>;
	using FAssetOwnedPayloadRelocatorHandle = uint64;
	ASSETCORE_API auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator,
		FModuleOwnedCallbackGate OwnerGate = {}
	)
		-> FAssetOwnedPayloadRelocatorHandle;
	ASSETCORE_API auto UnregisterAssetOwnedPayloadRelocator(
		FAssetOwnedPayloadRelocatorHandle Handle
	) -> void;

	// Receives committed relocation direction changes for transient editor and
	// cache state owned outside AssetCore, including Undo and Redo direction.
	// Observers cannot reject or roll back authored publication.
	class IAssetMoveObserver
	{
	public:
		virtual ~IAssetMoveObserver() = default;
		virtual auto OnAssetsRelocated(
			std::span<const FAssetRelocationMapping> Mappings
		) -> void = 0;
	};

	using FAssetMoveObserverHandle = uint64;
	ASSETCORE_API auto RegisterAssetMoveObserver(
		IAssetMoveObserver* Observer,
		FModuleOwnedCallbackGate OwnerGate = {}
	)
		-> FAssetMoveObserverHandle;
	ASSETCORE_API auto UnregisterAssetMoveObserver(
		FAssetMoveObserverHandle Handle
	) -> void;

	// Executes one prepared mutation without exposing revalidation, journal, or
	// compensation phases to its caller.
	class FAssetMutationTransaction
	{
	public:
		ASSETCORE_API auto GetSummary() const -> const FAssetMutationSummary&;
		ASSETCORE_API auto GetState() const -> EAssetMutationTransactionState;
		ASSETCORE_API auto GetLastResultDetails() const
			-> FAssetMutationResultDetails;
		ASSETCORE_API auto Commit() -> FAssetResult;
		ASSETCORE_API auto Undo() -> FAssetResult;
		ASSETCORE_API auto Redo() -> FAssetResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;

#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};

	// Failure seams are deterministic and one-shot so transaction-boundary tests
	// can prove compensation without changing production publication behavior.

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

	// Supplies the editor-owned physical half of a deletion. Each operation must
	// compensate its own partially completed filesystem work before returning failure.
	struct FAssetDeletionPhysicalTransition
	{
		std::function<FAssetResult()> Stage;
		std::function<FAssetResult()> Restore;
		std::function<bool()> IsRecoveryRequired;
	};

	// Opaque AssetCore contribution retained by the owning content transaction.
	// AssetCore owns validation, residency, registry publication, and ordering around
	// the caller-owned physical transition.
	class FAssetDeletionTransaction
	{
	public:
		ASSETCORE_API auto GetRegistryRevision() const -> uint64;
		ASSETCORE_API auto GetEntries() const
			-> std::span<const FAssetDeletionBatchEntry>;
		ASSETCORE_API auto GetWarnings() const
			-> std::span<const FAssetDeletionBatchWarning>;
		ASSETCORE_API auto GetState() const -> EAssetMutationTransactionState;
		ASSETCORE_API auto Commit(const FAssetDeletionPhysicalTransition& Transition)
			-> FAssetResult;
		ASSETCORE_API auto Undo(const FAssetDeletionPhysicalTransition& Transition)
			-> FAssetResult;
		ASSETCORE_API auto Redo(const FAssetDeletionPhysicalTransition& Transition)
			-> FAssetResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;

#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};

	using FAssetDeleteContributor = std::function<FAssetResult(const FAssetData&, const FAssetPackageInspection&, FAssetDeleteContribution&)>;
	using FAssetDeleteContributorHandle = uint64;
	// Cross-module extension point only for class-owned deletion companions or
	// reversible external state. Shared source inputs are not deletion companions
	// and must not install no-op contributors.
	ASSETCORE_API auto RegisterAssetDeleteContributor(
		DClass* Class,
		FAssetDeleteContributor Contributor,
		FModuleOwnedCallbackGate OwnerGate = {}
	) -> FAssetDeleteContributorHandle;
	ASSETCORE_API auto UnregisterAssetDeleteContributor(
		FAssetDeleteContributorHandle Handle
	) -> void;

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
		FAssetCompanionOwnership& OutOwnership
	) -> FAssetResult;

	// Owns the discovered asset index and its persistent snapshot state.
	ASSETCORE_API auto CreateAsset(
		const FAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset
	) -> FAssetResult;
	// Clones the complete persistent object graph into a new, unsaved package.
	// The caller may apply class-owned clone identity changes before SavePackage.
	ASSETCORE_API auto DuplicateAsset(
		const FAssetPath& SourcePath,
		const FAssetPath& DestinationPath,
		DObject*& OutAsset
	) -> FAssetResult;
	template<typename T>
	auto CreateAsset(const FAssetPath& Path, T*& OutAsset) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = CreateAsset(
			Path, T::StaticClass(), sizeof(T), Object
		);
		OutAsset = Result ? Cast<T>(Object) : nullptr;
		return Result;
	}

	ASSETCORE_API auto SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions& Options = {}
	) -> FAssetResult;
	ASSETCORE_API auto PrepareAssetRelocationTransaction(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetMutationSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction
	) -> FAssetResult;
	ASSETCORE_API auto PrepareRedirectorFixupTransaction(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction
	) -> FAssetResult;
	ASSETCORE_API auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult;
	ASSETCORE_API auto PrepareAssetDeletionTransaction(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionTransaction& OutTransaction,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers
	) -> FAssetResult;
} // namespace Durin::Asset
