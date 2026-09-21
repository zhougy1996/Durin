#include "Preview/StudioLighting.h"
#include "Actors/SkyLightActor.h"
#include "Asset/Load.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Level.h"
#include "Texture/TextureCube.h"

namespace Durin::Editor
{
    auto AddStudioSkyLight(DLevel& Level) -> bool
    {
        FObjectPath Path;
        if (!FObjectPath::TryCreate("/Engine/Renderer/DefaultStudioCube.DefaultStudioCube", Path)) return false;
        DTextureCube* Cube = nullptr;
        if (!(Cube = LoadObject<DTextureCube>(Path).value_or(nullptr))) return false;
        auto* Actor = Level.SpawnActor<ASkyLightActor>("StudioSkyLight");
        if (!Actor) return false;
        Actor->GetSkyLightComponent()->SetSource(ESkyLightSourceMode::SpecifiedCube, Cube);
        return true;
    }
}
