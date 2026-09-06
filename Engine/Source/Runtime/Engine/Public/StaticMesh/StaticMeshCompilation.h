#pragma once

#include "StaticMesh/StaticMeshBuild.h"
#include "Asset/AssetCompilingManager.h"

namespace Durin
{
	enum class EStaticMeshCompilationStatus : uint8 { Succeeded, Failed, Cancelled, Superseded };
	enum class EStaticMeshCompilationPriority : uint8 { Background, Interactive };
	enum class EStaticMeshCompilationPhase : uint8 { Queued, Building, Mailbox, Terminal };

	struct FStaticMeshCompilationDiagnostic
	{
		uint64 RequestId = 0;
		FObjectHandle Owner;
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
		std::string Message;
	};
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
		FStaticMeshImportedData Source;
		EStaticMeshCompilationPriority Priority = EStaticMeshCompilationPriority::Background;
		bool bPersistDerivedData = true;
		bool bMarkPackageDirty = true;
		// Owner thread only. Prepare a private provenance inner without changing live state.
		// Application validates it before mutation and installs its pointer within the refresh boundary.
		std::function<bool(DStaticMesh&, DAssetImportData*&, std::string&)> PreparePublication;
	};
	using FStaticMeshCompilationCompletion = std::function<void(const FStaticMeshCompilationDiagnostic&)>;

	ENGINE_API auto SubmitStaticMeshCompilation(DStaticMesh& Mesh, FStaticMeshCompilationRequest Request,
		std::string& OutError, FStaticMeshCompilationCompletion Completion = {}) -> bool;
	ENGINE_API auto CanJoinStaticMeshCompilation(const DStaticMesh& Mesh, const FStaticMeshImportedData& Source) -> bool;
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
