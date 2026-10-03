#pragma once

#include "CoreMinimal.h"
#include "Collision/CollisionDebugSubsystem.h"

inline auto RegisterWorldServicesForTests() -> void
{
	static const Durin::FWorldSubsystemRegistration CollisionDebug({.Type = Durin::DCollisionDebugSubsystem::StaticClass()});
}
