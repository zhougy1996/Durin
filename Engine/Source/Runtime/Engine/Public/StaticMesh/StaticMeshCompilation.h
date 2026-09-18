#pragma once

#include "StaticMesh/StaticMeshBuild.h"
#include "Asset/AssetCompilingManager.h"

namespace Durin
{
	enum class EStaticMeshCompilationStatus : uint8 { Succeeded, Failed, Cancelled, Superseded };
	enum class EStaticMeshCompilationPriority : uint8 { Background, Interactive };
	enum class EStaticMeshCompilationPhase : uint8 { Queued, Building, Mailbox, Terminal };

	struct FAssetResult;
	enum class EStaticMeshCompletionError : uint8 { None, Build, Application, PackageUnavailable, Save };
	struct FStaticMeshCompletionError
	{
		EStaticMeshCompletionError Code = EStaticMeshCompletionError::None;
		FObjectKey Owner;
		std::optional<FStaticMeshAuthoredBuildError> BuildCause;
		std::optional<FStaticMeshApplicationError> ApplicationCause;
		std::shared_ptr<const FAssetResult> SaveCause;
	};
	ENGINE_API auto FormatStaticMeshCompletionError(const FStaticMeshCompletionError& Error) -> std::string;

	struct FStaticMeshCompilationDiagnostic
	{
		uint64 RequestId = 0;
		FObjectKey Owner;
		EStaticMeshCompilationStatus Status = EStaticMeshCompilationStatus::Failed;
		EStaticMeshCompilationPhase Phase = EStaticMeshCompilationPhase::Queued;
		uint64 ReservedBytes = 0;
		FXxHash128 SourceIdentity;
		FStaticMeshBuildProviderDescriptor Descriptor;
		uint64 ProviderRegistration = 0;
		std::optional<FStaticMeshBuildObservation> Render;
		std::optional<FStaticMeshBuildObservation> Collision;
		uint64 CaptureNanoseconds = 0;
		uint64 WorkerNanoseconds = 0;
		uint64 PublicationNanoseconds = 0;
		FStaticMeshCompletionError Error;
		FStaticMeshPersistenceDiagnostic PersistenceDiagnostic;
	};
	ENGINE_API auto FormatStaticMeshCompilationDiagnostic(const FStaticMeshCompilationDiagnostic& Diagnostic) -> std::string;
	using FStaticMeshPublicationPreparation = std::function<FStaticMeshApplicationResult(DStaticMesh&, DAssetImportData*&)>;

	struct FStaticMeshCompilationManagerDiagnostics
	{
		uint32 OutstandingRecords = 0;
		uint32 RunningWorkers = 0;
		uint32 RetainedDiagnostics = 0;
		uint64 ReservedBytes = 0;
		bool bAcceptingRequests = false;
	};
	struct FStaticMeshCompilationRequest
	{
		FStaticMeshSource Source;
		EStaticMeshCompilationPriority Priority = EStaticMeshCompilationPriority::Background;
		bool bPersistDerivedData = true;
		bool bMarkPackageDirty = true;
		// Owner thread only. Prepare a private provenance inner without changing live state.
		// Application validates it before mutation and installs its pointer within the refresh boundary.
		FStaticMeshPublicationPreparation PreparePublication;
	};
	using FStaticMeshCompilationCompletion = std::function<void(const FStaticMeshCompilationDiagnostic&)>;

	enum class EStaticMeshSubmissionError : uint8
	{
		None, Unavailable, Owner, Source, Settings, CollisionSettings, MaterialSlots,
		ImportValidation, RequestBudget, AdmissionBudget, Provider
	};
	struct FStaticMeshSubmissionError
	{
		EStaticMeshSubmissionError Code = EStaticMeshSubmissionError::None;
		FObjectKey Owner;
		bool Accepting = false;
		bool OwnerValid = false;
		FXxHash128 SourceIdentity;
		float NormalizedSize = 0;
		uint64 SlotCount = 0;
		uint64 SlotIndex = 0;
		std::string SlotName;
		std::string SourceName;
		EBodySetupCollisionSourceMode CollisionMode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy CollisionPolicy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		uint64 RecordCount = 0;
		uint64 RecordLimit = 0;
		uint64 ReservedBytes = 0;
		uint64 RequestedBytes = 0;
		uint64 ByteLimit = 0;
		std::optional<FAssetImportDataError> ImportCause;
		std::optional<FStaticMeshBuildMemoryEstimate> MemoryCause;
		std::optional<EFeatureInvokeStatus> InvocationStatus;
		std::optional<FStaticMeshBuildProviderDescriptor> Descriptor;
	};
	struct FStaticMeshSubmissionResult
	{
		FStaticMeshSubmissionError Error;
		explicit operator bool() const { return Error.Code == EStaticMeshSubmissionError::None; }
	};
	ENGINE_API auto FormatStaticMeshSubmissionError(const FStaticMeshSubmissionError& Error) -> std::string;

	ENGINE_API auto SubmitStaticMeshCompilation(DStaticMesh& Mesh, FStaticMeshCompilationRequest Request,
		FStaticMeshCompilationCompletion Completion = {}) -> FStaticMeshSubmissionResult;
	ENGINE_API auto CanJoinStaticMeshCompilation(const DStaticMesh& Mesh, const FStaticMeshSource& Source) -> bool;
	ENGINE_API auto HasPendingStaticMeshSourceMutation(const DStaticMesh& Mesh) -> bool;
	ENGINE_API auto HasPendingStaticMeshCompilation(const DStaticMesh& Mesh) -> bool;
	ENGINE_API auto GetStaticMeshCompilationDiagnostic(const DStaticMesh& Mesh) -> FStaticMeshCompilationDiagnostic;
	ENGINE_API auto GetStaticMeshCompilationManagerDiagnostics() -> FStaticMeshCompilationManagerDiagnostics;
	// Invalidation is deferred: callbacks are dispatched by the owner-thread pump.
	ENGINE_API auto CancelStaticMeshCompilation(DStaticMesh& Mesh) -> void;
	ENGINE_API auto NotifyStaticMeshCompilationMutation(DStaticMesh& Mesh) -> void;

	namespace AssetPrivate
	{
		ENGINE_API auto CreateStaticMeshCompilingManager() -> std::shared_ptr<IAssetCompilingManager>;
		ENGINE_API auto SetStaticMeshCompilationPhaseHookForTests(
			std::function<void(uint64, EStaticMeshCompilationPhase)> Hook) -> void;
	}
}
