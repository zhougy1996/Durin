#include "Panels/DetailsPanelTargeting.h"

#include "Components/SceneComponent.h"
#include "Engine/Actor.h"

namespace Durin::Editor::Level::DetailsPanelTargeting
{
	auto ResolveDefaultComponent(AActor* Actor) -> DActorComponent*
	{
		return Actor ? Actor->GetRootComponent() : nullptr;
	}

	auto ResolveSelectedComponent(AActor* Actor, DActorComponent* SelectedComponent) -> DActorComponent*
	{
		if (!SelectedComponent) return nullptr;
		if (Actor && Actor->OwnsComponent(SelectedComponent)) return SelectedComponent;
		return ResolveDefaultComponent(Actor);
	}
} // namespace Durin::Editor::Level::DetailsPanelTargeting
