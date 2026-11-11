#pragma once

#include "CoreMinimal.h"

#include "Misc/CoreTypes.h"

namespace Durin
{
	class AActor;
}

namespace Durin::LevelEditor
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
} // namespace Durin::LevelEditor
