#pragma once
#include "CoreMinimal.h"
#include "Engine/PrimaryDataAsset.h"
#include "StaticMesh/StaticMesh.h"
#include "SandboxAPI.h"
#include "PlayerPawnDefaults.gen.h"

namespace Durin::Sandbox
{
    DCLASS()
    class DPlayerPawnDefaults final : public DPrimaryDataAsset
    {
        GENERATED_BODY()
    public:
        SANDBOX_API explicit DPlayerPawnDefaults(const FObjectInitializer& Initializer) : DPrimaryDataAsset(Initializer) {}

        DPROPERTY(Edit, Category = "Pawn")
        TObjectPtr<DStaticMesh> Mesh;
    };
}
