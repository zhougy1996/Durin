#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "Physics/PhysicsCookHelper.h"
namespace Durin::PhysicsPrivate
{
	ENGINE_API auto MakeCollisionBuildFunction() -> std::shared_ptr<const DerivedData::IBuildFunction>;
	ENGINE_API auto MakeCollisionInputResolver(FPhysicsCookInput Input) -> std::shared_ptr<const DerivedData::IBuildInputResolver>;
}
#endif
