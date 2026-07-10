#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

namespace Durin
{
	AStaticMeshActor::AStaticMeshActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		DURIN_INFO(STR("AStaticMeshActor::AStaticMeshActor"));
		StaticMeshComponent = NewObject<DStaticMeshComponent>(this, "DStaticMeshComponent");
		RootComponent = StaticMeshComponent;
	}

	auto AStaticMeshActor::GetStaticMeshComponent() const -> DStaticMeshComponent*
	{
		return StaticMeshComponent.Get();
	}
} // namespace Durin
