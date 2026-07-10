#pragma once

#include "DObject/ObjectPtr.h"

namespace Durin
{
	class AActor;
	class DWorld;
	class DLevel;

	struct FLevelEditorContext
	{
		DWorld* World = nullptr;
		DLevel* Level = nullptr;
		TObjectPtr<AActor> SelectedActor;

		auto Synchronize(DWorld* CurrentWorld) -> void;
		auto SelectActor(AActor* Actor) -> void;
		auto ClearSelection() -> void { SelectedActor = nullptr; }
	};
} // namespace Durin
