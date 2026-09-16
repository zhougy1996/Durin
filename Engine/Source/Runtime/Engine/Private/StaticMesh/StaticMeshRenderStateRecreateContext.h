#pragma once

#include "DObject/ObjectKey.h"
#include "EngineAPI.h"

namespace Durin
{
	class DStaticMesh;

	// Privately coordinates registered component render state while one mesh's
	// current render data is replaced.
	class FStaticMeshRenderStateRecreateContext
	{
	public:
		ENGINE_API explicit FStaticMeshRenderStateRecreateContext(DStaticMesh* StaticMesh);
		ENGINE_API ~FStaticMeshRenderStateRecreateContext();

		FStaticMeshRenderStateRecreateContext(
			const FStaticMeshRenderStateRecreateContext&) = delete;
		auto operator=(const FStaticMeshRenderStateRecreateContext&)
			-> FStaticMeshRenderStateRecreateContext& = delete;

	private:
		FObjectKey StaticMeshHandle;
		std::vector<FObjectKey> ComponentHandles;
	};
} // namespace Durin
