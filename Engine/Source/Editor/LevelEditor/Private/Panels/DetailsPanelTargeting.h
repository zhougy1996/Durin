#pragma once

namespace Durin
{
	class AActor;
	class DActorComponent;

	namespace DetailsPanelTargeting
	{
		auto ResolveDefaultComponent(AActor* Actor) -> DActorComponent*;
		auto ResolveSelectedComponent(AActor* Actor, DActorComponent* SelectedComponent) -> DActorComponent*;
	} // namespace DetailsPanelTargeting
} // namespace Durin
