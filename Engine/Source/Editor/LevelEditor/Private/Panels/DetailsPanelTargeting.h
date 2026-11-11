#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class AActor;
	class DActorComponent;
}

namespace Durin::LevelEditor
{

	namespace DetailsPanelTargeting
	{
		auto ResolveDefaultComponent(AActor* Actor) -> DActorComponent*;
		auto ResolveSelectedComponent(AActor* Actor, DActorComponent* SelectedComponent) -> DActorComponent*;
	} // namespace DetailsPanelTargeting
} // namespace Durin::LevelEditor