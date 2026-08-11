#include "DefaultGameMode.h"

#include "DefaultPlayerController.h"
#include "PlayerPawn.h"

namespace Durin::Sandbox
{
	ADefaultGameMode::ADefaultGameMode(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto ADefaultGameMode::GetPlayerControllerClass() const -> DClass*
	{
		return ADefaultPlayerController::StaticClass();
	}

	auto ADefaultGameMode::GetDefaultPawnClass() const -> DClass*
	{
		return APlayerPawn::StaticClass();
	}
}
