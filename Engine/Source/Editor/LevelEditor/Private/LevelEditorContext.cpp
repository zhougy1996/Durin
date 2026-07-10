#include "LevelEditorContext.h"

#include "Engine/World.h"
#include "Engine/Level.h"

namespace Durin
{
	auto FLevelEditorContext::Synchronize(DWorld* CurrentWorld) -> void
	{
		DLevel* CurrentLevel = CurrentWorld ? CurrentWorld->GetCurrentLevel() : nullptr;
		if (World != CurrentWorld || Level != CurrentLevel)
		{
			World = CurrentWorld;
			Level = CurrentLevel;
			ClearSelection();
			return;
		}

		AActor* Actor = SelectedActor.Get();
		if (Actor != nullptr && (World == nullptr || !World->ContainsActor(Actor)))
		{
			ClearSelection();
		}
	}

	auto FLevelEditorContext::SelectActor(AActor* Actor) -> void
	{
		SelectedActor = World != nullptr && World->ContainsActor(Actor) ? Actor : nullptr;
	}
} // namespace Durin
