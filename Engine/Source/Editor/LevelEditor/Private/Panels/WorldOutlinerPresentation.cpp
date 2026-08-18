#include "Panels/WorldOutlinerPresentation.h"

#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Actors/PlayerStart.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"

namespace Durin::Editor::Level
{
	namespace
	{
		struct FActorIconRule
		{
			const DClass* ActorClass = nullptr;
			EWorldOutlinerIcon Icon = EWorldOutlinerIcon::Actor;
		};
	} // namespace

	auto ClassifyWorldOutlinerActorIcon(AActor* Actor) -> EWorldOutlinerIcon
	{
		if (!Actor) return EWorldOutlinerIcon::Actor;

		const std::array Rules{
			FActorIconRule{ACameraActor::StaticClass(), EWorldOutlinerIcon::Camera},
			FActorIconRule{APlayerStart::StaticClass(), EWorldOutlinerIcon::PlayerStart},
			FActorIconRule{ADirectionalLightActor::StaticClass(), EWorldOutlinerIcon::DirectionalLight},
		};
		for (const FActorIconRule& Rule : Rules)
		{
			if (Actor->IsA(Rule.ActorClass)) return Rule.Icon;
		}
		if (Actor->FindComponentByClass<DStaticMeshComponent>()) return EWorldOutlinerIcon::StaticMesh;
		return EWorldOutlinerIcon::Actor;
	}
} // namespace Durin::Editor::Level
