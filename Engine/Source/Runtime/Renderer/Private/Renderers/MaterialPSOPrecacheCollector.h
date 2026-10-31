#pragma once

#include "CoreMinimal.h"
#include "PSOPrecacheMaterial.h"
#include "RenderResourceCreation.h"
#include "RendererAPI.h"

namespace Durin::RendererPrivate
{
    class FMeshPassProcessor;
    struct FMeshPassShaders;
    // Reusable native Shader/declaration cache. No material collection cursor or phase.
    class FMeshPSOPrecacheResources final
    {
    public:
        RENDERER_API FMeshPSOPrecacheResources();
        RENDERER_API ~FMeshPSOPrecacheResources();
        RENDERER_API auto Prepare(const FMeshPassProcessor& Processor, const FMaterialPSOPrecacheParams& Params,
            const FRenderResourceGeneration& Generation) -> std::expected<bool, std::string>;
        RENDERER_API auto GetShaders() const -> const FMeshPassShaders&;
        RENDERER_API auto GetDeclaration() const -> FRHIVertexDeclaration*;
    private:
        struct FState;
        std::unique_ptr<FState> State;
    };
    RENDERER_API auto CollectMaterialPSOs(const FMaterialPSOPrecacheParams& Params,
        FMaterialPSOPrecacheCollection& Collection, const FRenderResourceGeneration& Generation) -> std::expected<bool, std::string>;
}
