#pragma once

#include "Asset/AssetDefinitions.h"
#include "Asset/PackageGraphPreparationError.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "DObject/AssetPath.h"
#include "DObject/ObjectGraphReplacement.h"
#include "EngineAPI.h"

namespace Durin
{
	class DPackage;

	// A reload never reports partial publication. Failure codes describe the stage
	// that rejected the request while the original live graphs were still usable.
	enum class EPackageReloadFailure : uint8
	{
		None,
		Unsupported,
		Unsaved,
		Busy,
		Stale,
		BudgetExceeded,
		IoError,
		InvalidClosure,
		IncompatibleGraph,
		UnmappedReference,
		ParticipantRejected,
		ResourcePreparationFailed,
	};

	enum class EPackageReloadStatus : uint8
	{
		Pending,
		Succeeded,
		Failed,
		Cancelled,
	};

	enum class EPackageReloadStage : uint8
	{
		Preflight,
		Quiesce,
		ReadAndPrepare,
		PrepareReferences,
		PrepareRuntimeProducts,
		Revalidate,
		Commit,
		Publish,
		Retire,
	};

	enum class EPackageReloadFaultPoint : uint8
	{
		PreflightBudget,
		QuiesceSelected,
		ReadMain,
		ReadBulk,
		CreateSkeleton,
		ResolveDependency,
		ApplyValues,
		RestoreLedger,
		PreparePostLoad,
		PrepareReferences,
		PrepareNativeParticipant,
		PrepareHistory,
		PrepareRuntimeProduct,
		ReserveRenderPublish,
		RevalidateDisk,
		RevalidateReferencers,
		BeforeCommit,
	};

	enum class EPackageReloadReason : uint8
	{
		None, FileInspection, MissingOperation, AlreadyActive, Cancelled, CookedRuntime, EmptyRequest,
		InvalidPackage, PackageBudget, InvalidIdentity, UnsavedPackage, MissingFile, InjectedQuiesceFailure,
		InjectedMainReadFailure, InjectedBulkReadFailure, InjectedRuntimeFailure, InjectedReferenceFailure,
		InjectedDiskRevalidationFailure, InjectedFinalValidationFailure, ObserverException, Allocation,
		CallbackException, TextureBudget, VolumeBudget, FunctionPreparation, TexturePreparation,
		MaterialPreparation, UnsupportedClass, ResourceRead, GraphPreparation, Replacement,
		ResourceRevalidation, DependencyRelease,
	};

	struct FPackageReloadDiagnostic
	{
		FPackagePath PackagePath;
		std::string ObjectPath;
		EPackageReloadStage Stage = EPackageReloadStage::Preflight;
		EPackageReloadReason Reason = EPackageReloadReason::None;
		std::string ClassName;
		std::filesystem::path File;
		std::error_code SystemError;
		uint64 Actual = 0;
		uint64 Maximum = 0;
		std::string Message;
		std::vector<FMaterialProgramDiagnostic> MaterialCauses;
		std::optional<FMaterialCompileStatus> MaterialCompileStatus;
		std::vector<FMaterialCompileDiagnostic> MaterialCompileCauses;
	};

	ENGINE_API auto FormatPackageReloadDiagnostic(const FPackageReloadDiagnostic& Diagnostic) -> std::string;

	struct FPackageReloadBudget
	{
		uint64 MaximumPackages = 16;
		uint64 MaximumObjects = 65536;
		uint64 MaximumReferenceSlots = 1048576;
		uint64 MaximumRetainedCpuBytes = 512ull * 1024 * 1024;
		uint64 MaximumCandidateGpuBytes = 256ull * 1024 * 1024;
	};

	// Engine participants bridge native owners that cannot be discovered through
	// reflected writable slots. Their Prepare/Validate/Commit contract is the
	// CoreDObject atomic replacement contract.
	class IPackageReloadParticipant : public IObjectReplacementParticipant
	{
	public:
		~IPackageReloadParticipant() override = default;
	};

	enum class EPackageReloadReceiptState : uint8
	{
		Pending,
		Ready,
		Failed,
		Retired,
	};

	// A small, thread-safe receipt for participants that publish or retire work on
	// another thread. Ready means all fallible publication admission has completed.
	class FPackageReloadResourceReceipt
	{
	public:
		ENGINE_API auto GetState() const -> EPackageReloadReceiptState;
		ENGINE_API auto SetReady() -> bool;
		ENGINE_API auto SetFailed(FPackageReloadDiagnostic Error) -> bool;
		ENGINE_API auto SetRetired() -> bool;
		ENGINE_API auto GetFailure() const -> FPackageReloadDiagnostic;

	private:
		mutable std::mutex Mutex;
		EPackageReloadReceiptState State = EPackageReloadReceiptState::Pending;
		FPackageReloadDiagnostic Error;
	};

	struct FPackageReloadRequest
	{
		std::vector<DPackage*> Packages;
		std::vector<std::shared_ptr<IPackageReloadParticipant>> Participants;
		FPackageReloadBudget Budget;
		std::function<bool()> IsCancelled;
		// Deterministic validation seam. Package/object ordinals are stable within
		// the deduplicated request and no hook runs inside the noexcept commit.
		std::function<bool(EPackageReloadFaultPoint, uint64, uint64)> ShouldFail;
	};

	struct FPackageReloadResult
	{
		EPackageReloadStatus Status = EPackageReloadStatus::Pending;
		EPackageReloadFailure Failure = EPackageReloadFailure::None;
		std::vector<FPackageReloadDiagnostic> Diagnostics;

		auto Succeeded() const -> bool { return Status == EPackageReloadStatus::Succeeded; }
		explicit operator bool() const { return Succeeded(); }
	};

	// Owns the old graph until native and threaded consumers permit retirement.
	// ReloadPackages performs all synchronous preparation and publication work;
	// Poll/Wait complete delayed retirement without starting another request.
	class FPackageReloadOperation
	{
	public:
		ENGINE_API FPackageReloadOperation();
		ENGINE_API ~FPackageReloadOperation();
		FPackageReloadOperation(const FPackageReloadOperation&) = delete;
		auto operator=(const FPackageReloadOperation&) -> FPackageReloadOperation& = delete;
		ENGINE_API FPackageReloadOperation(FPackageReloadOperation&&) noexcept;
		ENGINE_API auto operator=(FPackageReloadOperation&&) noexcept -> FPackageReloadOperation&;
		ENGINE_API auto Poll() -> FPackageReloadResult;
		ENGINE_API auto Wait() -> FPackageReloadResult;
		ENGINE_API auto Cancel() -> void;
		ENGINE_API auto GetResult() const -> FPackageReloadResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;
		explicit FPackageReloadOperation(std::shared_ptr<FState> InState)
			: State(std::move(InState)) {}
		friend ENGINE_API auto ReloadPackages(const FPackageReloadRequest&)
			-> FPackageReloadOperation;
	};

	// GameThread only. Duplicate package inputs are coalesced and the complete
	// bounded request either commits together or preserves every original graph.
	ENGINE_API auto ReloadPackages(const FPackageReloadRequest& Request)
		-> FPackageReloadOperation;
}
