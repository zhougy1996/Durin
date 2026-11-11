#pragma once

#include "CoreMinimal.h"

#include "LevelEditorCustomizations.h"

namespace Durin::LevelEditor
{
	auto CreatePlayerStartActorVisualizer() -> std::shared_ptr<IActorEditorVisualizer>;
}
