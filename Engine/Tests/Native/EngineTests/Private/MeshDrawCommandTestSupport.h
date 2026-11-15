#pragma once

#include "CoreMinimal.h"
#include "Renderers/MeshPassProcessor.h"

namespace Durin::RendererPrivate
{
    inline auto AddTestMeshDrawCommand(FDynamicMeshDrawCommandStorage& Storage, FParallelMeshDrawCommandPass& Pass,
        FMeshDrawCommand Initializer, uint32 SourceDrawIndex) -> void
    {
        FPreparedStaticMeshPrimitive Primitive;
        FPreparedStaticMeshDraw Draw; Draw.ResolvedIndex = SourceDrawIndex;
        FResolvedStaticMeshView::FMaterialUniform Material;
        FMeshDrawCommandBuildInput Input{FRHICommandListImmediate::Get(), Primitive, Draw, {}, Material};
        FMeshDrawCommandBindingCache Cache;
        FDynamicPassMeshDrawListContext Context(Storage, Pass, Cache);
        auto& Command = Context.AddCommand(Initializer);
        Context.FinalizeCommand(Input, Command);
    }
}
