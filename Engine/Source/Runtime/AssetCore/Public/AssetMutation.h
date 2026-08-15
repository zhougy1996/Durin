#pragma once

#include "AssetLoad.h"

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
	ASSETCORE_API auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store,
		FModuleOwnedCallbackGate OwnerGate = {})
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

	#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetRuntimeState;
	#endif
	};

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
	ASSETCORE_API auto RegisterAssetMoveObserver(
		IAssetMoveObserver* Observer,
		FModuleOwnedCallbackGate OwnerGate = {})
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

	#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetRuntimeState;
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

	#if defined(DURIN_ASSETCORE_INTERNAL)
		friend class FAssetRuntimeState;
	#endif
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
	ASSETCORE_API auto CreateAsset(
		const FAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset) -> FAssetResult;
	template<typename T>
	auto CreateAsset(const FAssetPath& Path, T*& OutAsset) -> FAssetResult
	{
		static_assert(std::is_base_of_v<DObject, T>);
		DObject* Object = nullptr;
		FAssetResult Result = CreateAsset(
			Path, T::StaticClass(), sizeof(T), Object);
		OutAsset = Result ? Cast<T>(Object) : nullptr;
		return Result;
	}

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
}
