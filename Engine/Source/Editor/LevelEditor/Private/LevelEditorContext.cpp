#include "LevelEditorContext.h"

#include "Engine/World.h"

namespace Durin
{
	auto FLevelEditorContext::Synchronize(DWorld* CurrentWorld) -> void
	{
		if (World != CurrentWorld)
		{
			World = CurrentWorld;
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
