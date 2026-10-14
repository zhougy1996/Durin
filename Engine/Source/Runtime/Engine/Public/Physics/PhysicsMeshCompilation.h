#pragma once

#include "CoreMinimal.h"

#include "Physics/PhysicsMeshInputTask.h"
#include "Physics/BodySetupTypes.h"

namespace Durin
{
	class DBodySetup;
	class DObject;
	class IAssetCompilingManager;
	struct FPhysicsMeshCompilationManagerDiagnostics
	{
		uint32 OutstandingRecords = 0;
		uint32 AdmittedBuildCount = 0;
		uint64 ReservedBytes = 0;
		bool bAcceptingRequests = false;
	};
	ENGINE_API auto IsPhysicsMeshCompiling(const DBodySetup& Body) -> bool;
	ENGINE_API auto GetPhysicsMeshCompilationManagerDiagnostics() -> FPhysicsMeshCompilationManagerDiagnostics;
	namespace AssetPrivate
	{
		ENGINE_API auto CreatePhysicsMeshCompilingManager() -> std::shared_ptr<IAssetCompilingManager>;
		ENGINE_API auto CancelPhysicsMeshCompilationForOwner(DObject& Owner) -> void;
		ENGINE_API auto FinishPhysicsMeshCompilationForOwner(DObject& Owner) -> void;
		ENGINE_API auto SetPhysicsMeshCompilationHooksForTests(std::function<void()> BeforeBuild, std::function<void()> AfterBuild) -> void;
	}
	// Engine-owned scheduling backend; BodySetup owns the resource lifecycle.
	ENGINE_API auto SubmitPhysicsMeshCompilation(DBodySetup& Body, std::unique_ptr<FPhysicsMeshInputTask> InputTask,
		bool bPersistDerivedData, FOnAsyncPhysicsCookFinished Completion) -> std::expected<void, FPhysicsCookFailure>;
	ENGINE_API auto CancelPhysicsMeshCompilation(DBodySetup& Body) -> void;
	ENGINE_API auto FinishPhysicsMeshCompilation(DBodySetup& Body) -> void;
}
