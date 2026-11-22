#include "Preview/StudioLighting.h"
#include "Actors/SkyLightActor.h"
#include "Asset/Load.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Level.h"
#include "Texture/TextureCube.h"

namespace Durin
{
    auto AddStudioSkyLight(DLevel& Level) -> DSkyLightComponent*
    {
        FSoftObjectPath Path;
        if (!FSoftObjectPath::TryCreate("/Engine/Renderer/DefaultStudioCube.DefaultStudioCube", Path)) return nullptr;
        auto* Cube = LoadObject<DTextureCube>(Path);
        if (!Cube) return nullptr;
        auto* Actor = Level.SpawnActor<ASkyLightActor>("StudioSkyLight");
        if (!Actor) return nullptr;
        auto* Light = Actor->GetSkyLightComponent();
        Light->SetSource(ESkyLightSourceMode::SpecifiedCube, Cube);
        return Light;
    }
}
