#pragma once

#include "Engine/API.h"
#include "Engine/Actor.h"

#include "StaticMeshActor.gen.h"

namespace Doge
{
	class DStaticMeshComponent;

	DCLASS()
	class AStaticMeshActor : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API AStaticMeshActor();
	private:

		DStaticMeshComponent* StaticMeshComponent = nullptr;
	};
}