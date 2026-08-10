#pragma once

#include "Engine/Actor.h"

#include "GameMode.gen.h"

namespace Durin
{
	class APlayerStart;
	class DWorld;

	// Selects the native local-player roles and deterministic start used by a World gameplay session.
	DCLASS()
	class AGameMode : public AActor
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit AGameMode(const FObjectInitializer& ObjectInitializer);
		ENGINE_API virtual auto GetPlayerControllerClass() const -> DClass*;
		ENGINE_API virtual auto GetDefaultPawnClass() const -> DClass*;
		ENGINE_API virtual auto ChoosePlayerStart(DWorld& World) const -> APlayerStart*;
	};
}
