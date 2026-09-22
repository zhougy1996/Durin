#pragma once

#include <optional>
#include "EngineAPI.h"
#include "Physics/PhysicsCookFailure.h"
#include "DObject/DObjectFwd.h"
#include "DObject/ObjectMacros.h"
#include "BodySetupTypes.gen.h"

namespace Durin
{
	// Selects the CPU collision recipe for authored mesh geometry.
	DENUM()
	enum class EBodySetupCollisionSourceMode : uint8
	{
		None = 0,
		ConvexHullFromLOD0 = 1,
		TriangleMeshFromLOD0 = 2
	};

	// Selects which collision geometry participates in queries.
	DENUM()
	enum class EBodySetupCollisionQueryPolicy : uint8
	{
		SimpleOnly,
		ComplexOnly,
		SimpleAndComplex
	};

	enum class EPhysicsCookCompletionStatus : uint8 { Succeeded, Failed, Cancelled, Superseded };
	struct FPhysicsCookCompletionResult
	{
		EPhysicsCookCompletionStatus Status = EPhysicsCookCompletionStatus::Failed;
		// Present only for Failed; cancellation and supersession are terminal states.
		std::optional<FPhysicsCookFailure> Error;
	};
	using FOnAsyncPhysicsCookFinished = std::function<void(const FPhysicsCookCompletionResult&)>;
	enum class EPhysicsMeshApplyResult : uint8 { Applied, Superseded, Failed };

	enum class EPhysicsMeshBuildStatus : uint8 { Unavailable, Pending, Ready, Failed };
}
