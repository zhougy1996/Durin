#pragma once

#include "CoreMinimal.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildFunction.h"
#include "StaticMesh/StaticMeshBuildTypes.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin::StaticMeshPrivate
{
	ENGINE_API auto MakeSharedOutput(std::unique_ptr<FStaticMeshRenderData> Product, uint32 MaterialSlotCount,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<DerivedData::FBuildOutput, std::string>;
	ENGINE_API auto ValidateSharedOutput(const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {}) -> std::expected<void, std::string>;
	ENGINE_API auto AssembleSharedOutput(const DerivedData::FBuildOutput& Output,
		const std::function<bool()>& ShouldCancel = {})
		-> std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string>;
}
#endif
