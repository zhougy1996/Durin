#pragma once

#include "CoreMinimal.h"
#include "Misc/Name.h"

namespace Durin
{
	class AActor;
	class DEditorActorSubsystem;
	class DLevel;
	class DTextureCube;
	struct FActorEditContext;
}

namespace Durin::LevelEditor
{
	auto PlaceSkyBox(DEditorActorSubsystem& Editor, DLevel& Level, DTextureCube* Cube, FName Name,
		const FActorEditContext& Context) -> std::expected<AActor*, std::string>;
}
