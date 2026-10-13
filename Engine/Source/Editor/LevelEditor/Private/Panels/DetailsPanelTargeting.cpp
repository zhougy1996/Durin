#include "Panels/DetailsPanelTargeting.h"

#include "Engine/Actor.h"

namespace Durin::Editor::Level::DetailsPanelTargeting
{
	auto ResolveDefaultComponent(AActor*) -> DActorComponent*
	{
		return nullptr;
	}

	auto ResolveSelectedComponent(AActor* Actor, DActorComponent* SelectedComponent) -> DActorComponent*
	{
		if (!SelectedComponent) return nullptr;
		if (Actor && Actor->OwnsComponent(SelectedComponent)) return SelectedComponent;
		return ResolveDefaultComponent(Actor);
	}
} // namespace Durin::Editor::Level::DetailsPanelTargeting
