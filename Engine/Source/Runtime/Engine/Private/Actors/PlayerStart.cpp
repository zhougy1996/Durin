#include "Actors/PlayerStart.h"

#include "Components/SceneComponent.h"

namespace Durin
{
	APlayerStart::APlayerStart(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		SetRootComponent(CreateDefaultComponent<DSceneComponent>("Root"));
	}
}
