#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "StaticMesh/StaticMeshBuild.h"

namespace Durin::StaticMeshPrivate
{
	ENGINE_API auto RegisterBuildFunction(IMeshBuilderModule& Module) -> void;
	ENGINE_API auto MakeRenderBuildFunction(IMeshBuilderModule& Module) -> std::shared_ptr<const DerivedData::IBuildFunction>;
	ENGINE_API auto MakeRenderInputResolver(const FStaticMeshBuildRequest& Request) -> std::shared_ptr<const DerivedData::IBuildInputResolver>;
}
#endif
