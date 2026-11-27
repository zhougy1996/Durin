#pragma once

#include "CoreMinimal.h"
#include "Modules/ModularFeature.h"

namespace Durin::LevelEditor
{
	auto RegisterStaticMeshActorFactory() -> FModularFeatureRegistration;
}
