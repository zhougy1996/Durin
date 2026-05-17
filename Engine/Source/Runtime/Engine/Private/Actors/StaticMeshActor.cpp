#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

namespace Durin
{
	AStaticMeshActor::AStaticMeshActor()
	{
		DURIN_INFO(STR("AStaticMeshActor::AStaticMeshActor"));
		StaticMeshComponent = NewObject<DStaticMeshComponent>(this, "DStaticMeshComponent");
		RootComponent = StaticMeshComponent;
	}
}
