#pragma once

#include "CoreMinimal.h"

#include "Asset/AssetBuildTaskContext.h"
#include "StaticMesh/StaticMeshBuildFailure.h"

namespace Durin
{
	struct FStaticMeshRenderData;

	namespace StaticMeshPrivate
	{
		// Requires geometry and bounds prepared by shared-output assembly and restored runtime metadata.
		auto PrepareValidatedRenderData(FStaticMeshRenderData& Render,
			const FAssetBuildTaskContext& Control) -> std::expected<void, FStaticMeshBuildFailure>;
	}
}
