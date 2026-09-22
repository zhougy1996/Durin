#pragma once

#include "Physics/PhysicsMeshInputTask.h"
#include "Physics/BodySetupTypes.h"

namespace Durin
{
	class DBodySetup;
	// Engine-owned scheduling backend; BodySetup owns the resource lifecycle.
	ENGINE_API auto SubmitPhysicsMeshCompilation(DBodySetup& Body, std::unique_ptr<FPhysicsMeshInputTask> InputTask,
		bool bPersistDerivedData, FOnAsyncPhysicsCookFinished Completion) -> std::expected<void, FPhysicsCookFailure>;
	ENGINE_API auto CancelPhysicsMeshCompilation(DBodySetup& Body) -> void;
	ENGINE_API auto FinishPhysicsMeshCompilation(DBodySetup& Body) -> void;
}
