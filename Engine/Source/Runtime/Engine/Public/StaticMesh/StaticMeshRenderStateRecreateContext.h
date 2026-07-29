#pragma once

#include "DObject/ObjectHandle.h"
#include "EngineAPI.h"

namespace Durin
{
	class DStaticMesh;

	// Temporarily removes registered component render state while one mesh's
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
		FObjectHandle StaticMeshHandle;
		std::vector<FObjectHandle> ComponentHandles;
	};
} // namespace Durin
