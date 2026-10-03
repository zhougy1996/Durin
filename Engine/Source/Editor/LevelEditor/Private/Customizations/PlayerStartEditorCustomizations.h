#pragma once

#include "CoreMinimal.h"

#include "LevelEditorCustomizations.h"

namespace Durin::Editor::Level
{
	auto CreatePlayerStartActorVisualizer() -> std::shared_ptr<IActorEditorVisualizer>;
}
