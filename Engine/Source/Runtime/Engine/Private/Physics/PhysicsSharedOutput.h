#pragma once
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildOutput.h"
#include "Physics/PhysicsCookHelper.h"

namespace Durin::PhysicsPrivate
{
	ENGINE_API auto MakeSharedOutput(FCollisionCookedData Cooked,
		EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<DerivedData::FBuildOutput, std::string>;
	ENGINE_API auto ValidateSharedOutput(const DerivedData::FBuildOutput& Output,
		EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<void, std::string>;
	ENGINE_API auto AssembleSharedOutput(const DerivedData::FBuildOutput& Output,
		EBodySetupCollisionSourceMode Mode, EBodySetupCollisionQueryPolicy Policy,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<FPhysicsCookResult, std::string>;
}
#endif
