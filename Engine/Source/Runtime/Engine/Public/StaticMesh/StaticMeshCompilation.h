#pragma once

#include "CoreMinimal.h"

#include "StaticMesh/StaticMesh.h"
#include "EngineAPI.h"

namespace Durin
{
	class IAssetCompilingManager;

	enum class EStaticMeshCompilationStatus : uint8 { Succeeded, Failed, Cancelled };
	enum class EStaticMeshCompilationPhase : uint8 { Queued, Building, Mailbox, Terminal };

	struct FStaticMeshCompilationDiagnostic
	{
		uint64 RequestId = 0;
		FObjectKey Owner;
		// Populated only after completion has retired and reached Terminal.
		std::optional<EStaticMeshCompilationStatus> Status;
		EStaticMeshCompilationPhase Phase = EStaticMeshCompilationPhase::Queued;
		// Estimate only, not a retained-memory reservation.
		uint64 EstimatedWorkingSetBytes = 0;
		FXxHash128 SourceIdentity;
		uint64 RenderBuilderVersion = 0;
		std::string Error;
	};
	ENGINE_API auto FormatStaticMeshCompilationDiagnostic(const FStaticMeshCompilationDiagnostic& Diagnostic) -> std::string;
	struct FStaticMeshCompilationManagerDiagnostics
	{
		uint32 OutstandingRecords = 0;
		// CPU stages only; cache I/O waits do not occupy execution capacity.
		uint32 AdmittedBuildCount = 0;
		uint32 RetainedDiagnostics = 0;
		// Estimate only, not a retained-memory reservation.
		uint64 EstimatedWorkingSetBytes = 0;
		bool bAcceptingRequests = false;
	};
#if DURIN_WITH_EDITORONLY_DATA
	struct FStaticMeshCompilationRequest
	{
		FStaticMeshSource Source;
		// Optional authoring input, installed only after successful construction and validation.
		std::optional<std::vector<FMeshMaterialSlotDefinition>> PreparedMaterialSlots;
		EStaticMeshCompilationPriority Priority = EStaticMeshCompilationPriority::Background;
		bool bPersistDerivedData = true;
		bool bMarkPackageDirty = true;

	};
#endif

#if DURIN_WITH_EDITORONLY_DATA
	ENGINE_API auto CanJoinStaticMeshCompilation(const DStaticMesh& Mesh, const FStaticMeshSource& Source) -> bool;
#endif

	ENGINE_API auto HasPendingStaticMeshSourceMutation(const DStaticMesh& Mesh) -> bool;
	// Query a particular completion even after its owner is destroyed or superseded.
	// Returns RequestId == 0 after the bounded history entry has expired.
	ENGINE_API auto GetStaticMeshCompilationDiagnostic(uint64 RequestId) -> FStaticMeshCompilationDiagnostic;
	ENGINE_API auto GetStaticMeshCompilationManagerDiagnostics() -> FStaticMeshCompilationManagerDiagnostics;
	ENGINE_API auto NotifyStaticMeshCompilationMutation(DStaticMesh& Mesh) -> void;

	namespace AssetPrivate
	{
		auto IsStaticMeshCompilationMutationBlocked(const DStaticMesh& Mesh) -> bool;
		ENGINE_API auto CreateStaticMeshCompilingManager() -> std::shared_ptr<IAssetCompilingManager>;
		ENGINE_API auto SetStaticMeshCompilationPhaseHookForTests(
			std::function<void(uint64, EStaticMeshCompilationPhase)> Hook) -> void;
	}
}
