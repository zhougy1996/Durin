#pragma once

#include "CoreMinimal.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "DObject/ObjectLifecycle.h"
#include "Threading/RunnableThread.h"

namespace Durin::Editor
{
	inline auto CanEditActorLevel(const DLevel* Level) -> bool
	{
		if (!IsInGameThread() || !IsValid(Level) || Level->IsTemplateObject()) return false;
		auto* World = Level->GetWorld();
		return !World || (IsValid(World) && !World->IsShutdownRequested() && World->GetPlayState() == EWorldPlayState::Stopped);
	}
	inline auto CanEditActorOwner(const AActor* Owner) -> bool
	{
		if (!IsInGameThread() || !IsValid(Owner) || Owner->IsTemplateObject()
			|| Owner->IsBeingDestroyed() || Owner->HasBegunPlay()) return false;
		auto* Level = Cast<DLevel>(Owner->GetOuter());
		return CanEditActorLevel(Level) && Level->ContainsActor(Owner);
	}
}
