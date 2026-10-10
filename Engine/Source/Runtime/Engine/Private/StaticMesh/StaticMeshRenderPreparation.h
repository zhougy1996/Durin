#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	struct FStaticMeshRenderData;

	namespace StaticMeshPrivate
	{
		// Requires valid geometry. A missing optional acceleration keeps reference traversal.
		auto PrepareRenderRayQueries(FStaticMeshRenderData& Render,
			const std::function<bool()>& ShouldCancel = {}) -> bool;
	}
}
