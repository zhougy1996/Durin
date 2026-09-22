#pragma once

#include "EngineAPI.h"
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

	using FOnAsyncPhysicsCookFinished = std::function<void(bool)>;
	struct FPhysicsCookFailure;
	enum class EPhysicsMeshBuildError : uint8 { None, MissingCollisionSource, DerivedData, Publication };
	struct FPhysicsMeshBuildError
	{
		EPhysicsMeshBuildError Code = EPhysicsMeshBuildError::None;
		EBodySetupCollisionSourceMode Mode = EBodySetupCollisionSourceMode::None;
		EBodySetupCollisionQueryPolicy Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		std::shared_ptr<const FPhysicsCookFailure> DerivedDataCause;
	};
	ENGINE_API auto FormatPhysicsMeshBuildError(const FPhysicsMeshBuildError& Error) -> std::string;


	enum class EPhysicsMeshBuildStatus : uint8 { Unavailable, Pending, Ready, Failed };
}
