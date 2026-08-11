#pragma once

#include "LevelEditorCustomizations.h"

namespace Durin::Editor::Level
{
	auto CreatePlayerStartActorVisualizer() -> std::shared_ptr<IActorEditorVisualizer>;
}
