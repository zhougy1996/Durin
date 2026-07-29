#pragma once

#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	// Gives focused tests mutable construction access without exposing installed
	// render-data mutation through the runtime asset API.
	class FStaticMeshTestAccess
	{
	public:
		static auto GetMutableRenderData(DStaticMesh* Mesh)
			-> FStaticMeshRenderData*
		{
			return const_cast<FStaticMeshRenderData*>(
				Mesh ? Mesh->GetRenderData() : nullptr);
		}
	};
}
