#include "Actors/StaticMeshActor.h"

#include "Components/StaticMeshComponent.h"

namespace Durin
{
	AStaticMeshActor::AStaticMeshActor(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		StaticMeshComponent = CreateDefaultComponent<DStaticMeshComponent>("DStaticMeshComponent");
		RootComponent = StaticMeshComponent;
	}

	auto AStaticMeshActor::GetStaticMeshComponent() const -> DStaticMeshComponent*
	{
		return StaticMeshComponent.Get();
	}
} // namespace Durin
