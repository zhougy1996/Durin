#pragma once
#include "CoreMinimal.h"
#include "PluginFixtureEditorAPI.h"
#include "Engine/PrimaryDataAsset.h"
#include "FixtureEditorAsset.gen.h"

namespace Durin::PluginFixture
{
    DCLASS()
    class DEditorFixtureAsset : public DPrimaryDataAsset
    {
        GENERATED_BODY()
    public:
        PLUGINFIXTUREEDITOR_API explicit DEditorFixtureAsset(const FObjectInitializer& Initializer)
            : DPrimaryDataAsset(Initializer) {}
        auto IsEditorOnly() const -> bool override { return true; }
    };
}
