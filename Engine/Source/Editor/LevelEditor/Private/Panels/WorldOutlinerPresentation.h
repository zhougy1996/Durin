#pragma once

#include "Misc/CoreTypes.h"

namespace Durin
{
	class AActor;
}

namespace Durin::Editor::Level
{
	// Semantic icon roles keep Actor classification independent from the glyph catalog.
	enum class EWorldOutlinerIcon : uint8
	{
		Actor,
		Camera,
		DirectionalLight,
		Level,
		PlayerStart,
		StaticMesh,
	};

	auto ClassifyWorldOutlinerActorIcon(AActor* Actor) -> EWorldOutlinerIcon;
} // namespace Durin::Editor::Level
